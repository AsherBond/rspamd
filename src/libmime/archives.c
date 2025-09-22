/*
 * Copyright 2024 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "message.h"
#include "task.h"
#include "archives.h"
#include "libmime/mime_encoding.h"
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>
#include <unicode/ucnv.h>

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#endif

#define msg_debug_archive(...) rspamd_conditional_debug_fast(NULL, NULL,                                                 \
															 rspamd_archive_log_id, "archive", task->task_pool->tag.uid, \
															 G_STRFUNC,                                                  \
															 __VA_ARGS__)
#define msg_debug_archive_taskless(...) rspamd_conditional_debug_fast(NULL, NULL,                             \
																	  rspamd_archive_log_id, "archive", NULL, \
																	  G_STRFUNC,                              \
																	  __VA_ARGS__)

INIT_LOG_MODULE(archive)

static GQuark
rspamd_archives_err_quark(void)
{
	static GQuark q = 0;
	if (G_UNLIKELY(q == 0)) {
		q = g_quark_from_static_string("archives");
	}

	return q;
}

static void
rspamd_archive_dtor(gpointer p)
{
	struct rspamd_archive *arch = p;
	struct rspamd_archive_file *f;
	unsigned int i;

	for (i = 0; i < arch->files->len; i++) {
		f = g_ptr_array_index(arch->files, i);

		if (f->fname) {
			g_string_free(f->fname, TRUE);
		}

		g_free(f);
	}

	g_ptr_array_free(arch->files, TRUE);
}

static inline guint16
rspamd_zip_time_dos(time_t t)
{
	struct tm lt;

	if (t == 0) {
		t = time(NULL);
	}

	(void) localtime_r(&t, &lt);

	guint16 dos_time = ((guint16) (lt.tm_hour & 0x1f) << 11) |
					   ((guint16) (lt.tm_min & 0x3f) << 5) |
					   ((guint16) ((lt.tm_sec / 2) & 0x1f));

	return dos_time;
}

static inline guint16
rspamd_zip_date_dos(time_t t)
{
	struct tm lt;

	if (t == 0) {
		t = time(NULL);
	}

	(void) localtime_r(&t, &lt);

	int year = lt.tm_year + 1900;
	if (year < 1980) {
		year = 1980; /* DOS date epoch */
	}

	guint16 dos_date = ((guint16) ((year - 1980) & 0x7f) << 9) |
					   ((guint16) ((lt.tm_mon + 1) & 0x0f) << 5) |
					   ((guint16) (lt.tm_mday & 0x1f));

	return dos_date;
}

static inline void
rspamd_ba_append_u16le(GByteArray *ba, guint16 v)
{
	union {
		guint16 u16;
		unsigned char b[2];
	} u;

	u.u16 = GUINT16_TO_LE(v);
	g_byte_array_append(ba, u.b, sizeof(u.b));
}

static inline void
rspamd_ba_append_u32le(GByteArray *ba, guint32 v)
{
	union {
		guint32 u32;
		unsigned char b[4];
	} u;

	u.u32 = GUINT32_TO_LE(v);
	g_byte_array_append(ba, u.b, sizeof(u.b));
}

static gboolean
rspamd_zip_deflate_alloc(const unsigned char *in,
						 gsize inlen,
						 unsigned char **outbuf,
						 gsize *outlen)
{
	int rc;
	z_stream strm;

	memset(&strm, 0, sizeof(strm));
	/* raw DEFLATE stream for ZIP */
	rc = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
					  -MAX_WBITS, MAX_MEM_LEVEL - 1, Z_DEFAULT_STRATEGY);

	if (rc != Z_OK) {
		return FALSE;
	}

	/* Compute upper bound and allocate */
	uLong bound = deflateBound(&strm, (uLong) inlen);
	unsigned char *obuf = g_malloc(bound);

	strm.next_in = (unsigned char *) in;
	strm.avail_in = inlen;
	strm.next_out = obuf;
	strm.avail_out = bound;

	rc = deflate(&strm, Z_FINISH);

	if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
		deflateEnd(&strm);
		g_free(obuf);
		return FALSE;
	}

	*outlen = bound - strm.avail_out;
	*outbuf = obuf;
	deflateEnd(&strm);

	return TRUE;
}

static gboolean
rspamd_zip_validate_name(const char *name)
{
	if (name == NULL || *name == '\0') {
		return FALSE;
	}
	/* Disallow absolute paths and parent traversals */
	if (name[0] == '/' || name[0] == '\\') {
		return FALSE;
	}
	if (strstr(name, "..") != NULL) {
		return FALSE;
	}
	if (strchr(name, ':') != NULL) {
		return FALSE;
	}

	return TRUE;
}

static void
rspamd_zip_write_local_header(GByteArray *zip,
							  const char *name,
							  guint16 ver_needed,
							  guint16 gp_flags,
							  guint16 method,
							  time_t mtime,
							  guint32 crc,
							  guint32 csize,
							  guint32 usize,
							  guint16 extra_len)
{
	/* Local file header */
	/* signature */
	rspamd_ba_append_u32le(zip, 0x04034b50);
	/* version needed to extract */
	rspamd_ba_append_u16le(zip, ver_needed);
	/* general purpose bit flag */
	rspamd_ba_append_u16le(zip, gp_flags);
	/* compression method */
	rspamd_ba_append_u16le(zip, method);
	/* last mod file time/date */
	rspamd_ba_append_u16le(zip, rspamd_zip_time_dos(mtime));
	rspamd_ba_append_u16le(zip, rspamd_zip_date_dos(mtime));
	/* CRC-32 */
	rspamd_ba_append_u32le(zip, crc);
	/* compressed size */
	rspamd_ba_append_u32le(zip, csize);
	/* uncompressed size */
	rspamd_ba_append_u32le(zip, usize);
	/* file name length */
	rspamd_ba_append_u16le(zip, (guint16) strlen(name));
	/* extra field length */
	rspamd_ba_append_u16le(zip, extra_len);
	/* file name */
	g_byte_array_append(zip, (const guint8 *) name, strlen(name));
}

static void
rspamd_zip_write_central_header(GByteArray *cd,
								const char *name,
								guint16 ver_needed,
								guint16 gp_flags,
								guint16 method,
								time_t mtime,
								guint32 crc,
								guint32 csize,
								guint32 usize,
								guint32 lfh_offset,
								guint32 mode,
								guint16 extra_len)
{
	/* Central directory file header */
	rspamd_ba_append_u32le(cd, 0x02014b50);
	/* version made by: 3 (UNIX) << 8 | 20 */
	rspamd_ba_append_u16le(cd, (guint16) ((3 << 8) | 20));
	/* version needed to extract */
	rspamd_ba_append_u16le(cd, ver_needed);
	/* general purpose bit flag */
	rspamd_ba_append_u16le(cd, gp_flags);
	/* compression method */
	rspamd_ba_append_u16le(cd, method);
	/* time/date */
	rspamd_ba_append_u16le(cd, rspamd_zip_time_dos(mtime));
	rspamd_ba_append_u16le(cd, rspamd_zip_date_dos(mtime));
	/* CRC and sizes */
	rspamd_ba_append_u32le(cd, crc);
	rspamd_ba_append_u32le(cd, csize);
	rspamd_ba_append_u32le(cd, usize);
	/* name len, extra len, comment len */
	rspamd_ba_append_u16le(cd, (guint16) strlen(name));
	rspamd_ba_append_u16le(cd, extra_len);
	rspamd_ba_append_u16le(cd, 0);
	/* disk number start, internal attrs */
	rspamd_ba_append_u16le(cd, 0);
	rspamd_ba_append_u16le(cd, 0);
	/* external attrs: UNIX perms in upper 16 bits */
	guint32 xattr = (mode ? mode : 0644);
	xattr = (xattr & 0xFFFF) << 16;
	rspamd_ba_append_u32le(cd, xattr);
	/* relative offset of local header */
	rspamd_ba_append_u32le(cd, lfh_offset);
	/* file name */
	g_byte_array_append(cd, (const guint8 *) name, strlen(name));
}

#define ZIP_AES_EXTRA_ID 0x9901

static void
rspamd_zip_write_extra_aes(GByteArray *ba, guint16 vendor_version, guint8 strength, guint16 actual_method)
{
	/* Extra field header id and size */
	rspamd_ba_append_u16le(ba, ZIP_AES_EXTRA_ID);
	/* data size = 7 */
	rspamd_ba_append_u16le(ba, 7);
	/* Vendor version */
	rspamd_ba_append_u16le(ba, vendor_version);
	/* Vendor ID 'AE' */
	const guint8 vid[2] = {'A', 'E'};
	g_byte_array_append(ba, vid, sizeof(vid));
	/* Strength */
	g_byte_array_append(ba, &strength, 1);
	/* Actual compression method */
	rspamd_ba_append_u16le(ba, actual_method);
}

GByteArray *
rspamd_archives_zip_write(const struct rspamd_zip_file_spec *files,
						  gsize nfiles,
						  const char *password,
						  GError **err)
{
	GByteArray *zip = NULL, *cd = NULL;
	GQuark q = rspamd_archives_err_quark();

	if (files == NULL || nfiles == 0) {
		g_set_error(err, q, EINVAL, "no files to archive");
		return NULL;
	}

	zip = g_byte_array_new();
	cd = g_byte_array_new();

	for (gsize i = 0; i < nfiles; i++) {
		const struct rspamd_zip_file_spec *f = &files[i];
		if (!rspamd_zip_validate_name(f->name)) {
			g_set_error(err, q, EINVAL, "invalid zip entry name: %s", f->name ? f->name : "(null)");
			g_byte_array_free(cd, TRUE);
			g_byte_array_free(zip, TRUE);
			return NULL;
		}

		guint32 crc = crc32(0L, Z_NULL, 0);
		crc = crc32(crc, f->data, f->len);
		guint16 method = 8;            /* deflate */
		guint16 gp_flags = (1u << 11); /* UTF-8 */
		guint16 ver_needed = 20;       /* default */
		const gboolean use_aes = (password != NULL && *password != '\0');

		/* actual method will be decided after deflate; default is deflate */

		guint16 extra_len = 0;
		guint16 actual_method = method;
		guint32 csize_for_header = 0;
		const guint8 aes_strength = 0x03;      /* AES-256 */
		const guint16 aes_vendor_ver = 0x0002; /* AE-2 */
		guint8 salt_len = 0;
		if (use_aes) {
			/* Per APPNOTE: method=99 (0x63), AES extra 0x9901 in both headers */
			ver_needed = MAX(ver_needed, (guint16) 51);
			gp_flags |= 1u; /* encrypted */
			method = 99;
			extra_len = 2 /*id*/ + 2 /*size*/ + 7 /*payload*/;
			/* salt length by strength */
			salt_len = (aes_strength == 0x01 ? 8 : (aes_strength == 0x02 ? 12 : 16));
			/* compressed size will be computed after deflate/encrypt */
			/* CRC-32 not used with AES: set to 0 */
			crc = 0;
		}

		guint32 lfh_off = zip->len;
		rspamd_zip_write_local_header(zip, f->name, ver_needed, gp_flags, method, f->mtime, crc,
									  csize_for_header,
									  (guint32) f->len, extra_len);
		if (use_aes) {
			/* Write AES extra for local header */
			rspamd_zip_write_extra_aes(zip, aes_vendor_ver, aes_strength, actual_method);

#ifdef HAVE_OPENSSL
			/* Derive keys and encrypt: PBKDF2-HMAC-SHA1 per AE-2 */
			unsigned char salt[16];
			if (RAND_bytes(salt, salt_len) != 1) {
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "cannot generate AES salt");
				return NULL;
			}

			/* key sizes by strength */
			int klen = (aes_strength == 0x01 ? 16 : (aes_strength == 0x02 ? 24 : 32));
			int dklen = klen * 2 + 2;
			unsigned char *dk = g_malloc(dklen);
			if (PKCS5_PBKDF2_HMAC(password, (int) strlen(password), salt, salt_len, 1000, EVP_sha1(), dklen, dk) != 1) {
				g_free(dk);
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "PBKDF2(HMAC-SHA1) failed");
				return NULL;
			}
			unsigned char *ekey = dk;          /* klen */
			unsigned char *akey = dk + klen;   /* klen */
			unsigned char *pv = dk + klen * 2; /* 2 bytes */

			/* Append salt and password verification value */
			g_byte_array_append(zip, salt, salt_len);
			g_byte_array_append(zip, pv, 2);

			/* AES-CTR encrypt */
			EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
			const EVP_CIPHER *cipher = (klen == 16 ? EVP_aes_128_ctr() : (klen == 24 ? EVP_aes_192_ctr() : EVP_aes_256_ctr()));
			unsigned char iv[16];
			memset(iv, 0, sizeof(iv)); /* WinZip AES uses counter mode starting at 0 */
			if (EVP_EncryptInit_ex(cctx, cipher, NULL, ekey, iv) != 1) {
				EVP_CIPHER_CTX_free(cctx);
				rspamd_explicit_memzero(dk, dklen);
				g_free(dk);
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "AES-CTR init failed");
				return NULL;
			}

			/* Deflate directly into zip and encrypt in-place */
			z_stream zst;
			memset(&zst, 0, sizeof(zst));
			if (deflateInit2(&zst, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL - 1, Z_DEFAULT_STRATEGY) != Z_OK) {
				EVP_CIPHER_CTX_free(cctx);
				rspamd_explicit_memzero(dk, dklen);
				g_free(dk);
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "deflateInit2 failed");
				return NULL;
			}
			uLong bound = deflateBound(&zst, (uLong) f->len);
			deflateEnd(&zst);
			/* Append salt+pv already written; now reserve deflate bound */
			gsize plain_off = zip->len;
			g_byte_array_set_size(zip, zip->len + bound);
			unsigned char *plain_ptr = zip->data + plain_off;
			memset(&zst, 0, sizeof(zst));
			if (deflateInit2(&zst, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL - 1, Z_DEFAULT_STRATEGY) != Z_OK) {
				EVP_CIPHER_CTX_free(cctx);
				rspamd_explicit_memzero(dk, dklen);
				g_free(dk);
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "deflateInit2 failed");
				return NULL;
			}
			zst.next_in = (unsigned char *) f->data;
			zst.avail_in = f->len;
			zst.next_out = plain_ptr;
			zst.avail_out = bound;
			int rc = deflate(&zst, Z_FINISH);
			if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
				deflateEnd(&zst);
				EVP_CIPHER_CTX_free(cctx);
				rspamd_explicit_memzero(dk, dklen);
				g_free(dk);
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "deflate failed");
				return NULL;
			}
			gsize produced = bound - zst.avail_out;
			deflateEnd(&zst);
			if (produced >= f->len) {
				/* fallback to store */
				g_byte_array_set_size(zip, plain_off);
				g_byte_array_set_size(zip, zip->len + f->len);
				memcpy(zip->data + plain_off, f->data, f->len);
				produced = f->len;
				actual_method = 0;
			}

			/* Encrypt in place */
			gsize enc_before = plain_off;
			unsigned char *enc_ptr = zip->data + enc_before;
			int outl = 0, finl = 0;
			if (EVP_EncryptUpdate(cctx, enc_ptr, &outl, enc_ptr, (int) produced) != 1 ||
				EVP_EncryptFinal_ex(cctx, enc_ptr + outl, &finl) != 1) {
				EVP_CIPHER_CTX_free(cctx);
				rspamd_explicit_memzero(dk, dklen);
				g_free(dk);
				g_byte_array_free(cd, TRUE);
				g_byte_array_free(zip, TRUE);
				g_set_error(err, q, EIO, "AES-CTR encrypt failed");
				return NULL;
			}
			EVP_CIPHER_CTX_free(cctx);
			/* shrink to ciphertext size */
			g_byte_array_set_size(zip, enc_before + outl + finl);

			/* HMAC-SHA1 over ciphertext */
			unsigned char mac[EVP_MAX_MD_SIZE];
			unsigned int maclen = 0;
			HMAC(EVP_sha1(), akey, klen, zip->data + enc_before, (int) (zip->len - enc_before), mac, &maclen);
			/* append first 10 bytes */
			g_byte_array_append(zip, mac, 10);

			/* cleanup */
			rspamd_explicit_memzero(dk, dklen);
			g_free(dk);

			/* Patch local header: compressed size and actual method in AES extra */
			csize_for_header = (guint32) (salt_len + 2 + (outl + finl) + 10);
			/* compressed size at offset lfh_off + 18 */
			guint32 *p32 = (guint32 *) (zip->data + lfh_off + 18);
			*p32 = GUINT32_TO_LE(csize_for_header);
			/* patch actual method in AES extra (last 2 bytes of AES extra payload) */
			guint16 *p16 = (guint16 *) (zip->data + lfh_off + 30 + (guint32) strlen(f->name) + 9);
			*p16 = GUINT16_TO_LE(actual_method);
#else
			g_byte_array_free(cdata, TRUE);
			g_byte_array_free(cd, TRUE);
			g_byte_array_free(zip, TRUE);
			g_set_error(err, q, ENOTSUP, "AES-CTR encryption requires OpenSSL");
			return NULL;
#endif
		}
		else {
			/* Not encrypted: deflate directly into zip, fallback to store */
			z_stream zst;
			memset(&zst, 0, sizeof(zst));
			if (deflateInit2(&zst, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL - 1, Z_DEFAULT_STRATEGY) != Z_OK) {
				g_set_error(err, q, EIO, "deflateInit2 failed");
				return NULL;
			}
			uLong bound = deflateBound(&zst, (uLong) f->len);
			deflateEnd(&zst);
			gsize off = zip->len;
			g_byte_array_set_size(zip, zip->len + bound);
			unsigned char *outp = zip->data + off;
			memset(&zst, 0, sizeof(zst));
			if (deflateInit2(&zst, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL - 1, Z_DEFAULT_STRATEGY) != Z_OK) {
				g_set_error(err, q, EIO, "deflateInit2 failed");
				return NULL;
			}
			zst.next_in = (unsigned char *) f->data;
			zst.avail_in = f->len;
			zst.next_out = outp;
			zst.avail_out = bound;
			int rc = deflate(&zst, Z_FINISH);
			if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
				deflateEnd(&zst);
				g_set_error(err, q, EIO, "deflate failed");
				return NULL;
			}
			gsize produced = bound - zst.avail_out;
			deflateEnd(&zst);
			if (produced >= f->len) {
				/* store */
				g_byte_array_set_size(zip, off);
				g_byte_array_set_size(zip, zip->len + f->len);
				memcpy(zip->data + off, f->data, f->len);
				produced = f->len;
				method = 0;
				/* patch method in local header (offset +8) */
				guint16 *pm = (guint16 *) (zip->data + lfh_off + 8);
				*pm = GUINT16_TO_LE(method);
			}
			else {
				g_byte_array_set_size(zip, off + produced);
			}
			csize_for_header = (guint32) produced;
			/* patch CRC (offset +14) and compressed size (offset +18) */
			guint32 *p32 = (guint32 *) (zip->data + lfh_off + 14);
			*p32 = GUINT32_TO_LE(crc);
			p32 = (guint32 *) (zip->data + lfh_off + 18);
			*p32 = GUINT32_TO_LE(csize_for_header);
		}

		rspamd_zip_write_central_header(cd, f->name, ver_needed, gp_flags, method, f->mtime, crc,
										csize_for_header,
										(guint32) f->len,
										lfh_off, f->mode, extra_len);
		if (use_aes) {
			rspamd_zip_write_extra_aes(cd, aes_vendor_ver, aes_strength, actual_method);
		}

		guint64 logged_csize = (guint64) csize_for_header;
		msg_debug_archive_taskless("zip: added entry '%s' (usize=%L, csize=%L, method=%s)",
								   f->name, (int64_t) f->len, (int64_t) logged_csize,
								   method == 0 ? "store" : "deflate");
	}

	/* Central directory start */
	guint32 cd_start = zip->len;
	g_byte_array_append(zip, cd->data, cd->len);
	guint32 cd_size = cd->len;
	g_byte_array_free(cd, TRUE);

	/* EOCD */
	rspamd_ba_append_u32le(zip, 0x06054b50);
	/* disk numbers */
	rspamd_ba_append_u16le(zip, 0);
	rspamd_ba_append_u16le(zip, 0);
	/* total entries on this disk / total entries */
	rspamd_ba_append_u16le(zip, (guint16) nfiles);
	rspamd_ba_append_u16le(zip, (guint16) nfiles);
	/* size of central directory */
	rspamd_ba_append_u32le(zip, cd_size);
	/* offset of central directory */
	rspamd_ba_append_u32le(zip, cd_start);
	/* zip comment length */
	rspamd_ba_append_u16le(zip, 0);

	msg_debug_archive_taskless("zip: created archive (%L bytes)", (int64_t) zip->len);
	return zip;
}

GByteArray *
rspamd_archives_encrypt_aes256_cbc(const unsigned char *in,
								   gsize inlen,
								   const char *password,
								   GError **err)
{
#ifndef HAVE_OPENSSL
	(void) in;
	(void) inlen;
	(void) password;
	GQuark q = rspamd_archives_err_quark();
	g_set_error(err, q, ENOTSUP, "OpenSSL is not available");
	return NULL;
#else
	GQuark q = rspamd_archives_err_quark();
	unsigned char salt[16];
	unsigned char iv[16];
	unsigned char key[32];
	const int kdf_iters = 100000;
	GByteArray *out = NULL;
	EVP_CIPHER_CTX *ctx = NULL;

	if (password == NULL || *password == '\0') {
		g_set_error(err, q, EINVAL, "empty password");
		return NULL;
	}

	if (RAND_bytes(salt, sizeof(salt)) != 1 || RAND_bytes(iv, sizeof(iv)) != 1) {
		g_set_error(err, q, EIO, "cannot generate random salt/iv: %s", ERR_error_string(ERR_get_error(), NULL));
		return NULL;
	}

	if (PKCS5_PBKDF2_HMAC(password, (int) strlen(password), salt, (int) sizeof(salt),
						  kdf_iters, EVP_sha256(), (int) sizeof(key), key) != 1) {
		g_set_error(err, q, EIO, "PBKDF2 failed: %s", ERR_error_string(ERR_get_error(), NULL));
		return NULL;
	}

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		g_set_error(err, q, ENOMEM, "cannot alloc cipher ctx");
		rspamd_explicit_memzero(key, sizeof(key));
		return NULL;
	}

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
		g_set_error(err, q, EIO, "cipher init failed: %s", ERR_error_string(ERR_get_error(), NULL));
		EVP_CIPHER_CTX_free(ctx);
		rspamd_explicit_memzero(key, sizeof(key));
		return NULL;
	}

	/* Prepare output: magic + salt + iv + ciphertext; write directly into GByteArray */
	const char magic[8] = {'R', 'Z', 'A', 'E', '0', '0', '0', '1'};
	out = g_byte_array_sized_new(8 + sizeof(salt) + sizeof(iv) + inlen + 32);
	g_byte_array_append(out, (const guint8 *) magic, sizeof(magic));
	g_byte_array_append(out, salt, sizeof(salt));
	g_byte_array_append(out, iv, sizeof(iv));

	gsize before = out->len;
	g_byte_array_set_size(out, out->len + inlen + EVP_CIPHER_block_size(EVP_aes_256_cbc()));
	unsigned char *cptr = out->data + before;
	int outlen = 0;

	if (EVP_EncryptUpdate(ctx, cptr, &outlen, in, (int) inlen) != 1) {
		g_set_error(err, q, EIO, "encrypt update failed: %s", ERR_error_string(ERR_get_error(), NULL));
		EVP_CIPHER_CTX_free(ctx);
		rspamd_explicit_memzero(key, sizeof(key));
		g_byte_array_set_size(out, before);
		g_byte_array_free(out, TRUE);
		return NULL;
	}

	int fin = 0;
	if (EVP_EncryptFinal_ex(ctx, cptr + outlen, &fin) != 1) {
		g_set_error(err, q, EIO, "encrypt final failed: %s", ERR_error_string(ERR_get_error(), NULL));
		EVP_CIPHER_CTX_free(ctx);
		rspamd_explicit_memzero(key, sizeof(key));
		g_byte_array_set_size(out, before);
		g_byte_array_free(out, TRUE);
		return NULL;
	}

	g_byte_array_set_size(out, before + outlen + fin);
	EVP_CIPHER_CTX_free(ctx);
	rspamd_explicit_memzero(key, sizeof(key));

	msg_info("zip: AES-256-CBC envelope created (PBKDF2-SHA256 iters=%d)", kdf_iters);
	return out;
#endif
}

static bool
rspamd_archive_file_try_utf(struct rspamd_task *task,
							struct rspamd_archive *arch,
							struct rspamd_archive_file *fentry,
							const char *in, gsize inlen)
{
	const char *charset = NULL, *p, *end;
	GString *res;

	charset = rspamd_mime_charset_find_by_content(in, inlen, TRUE);

	if (charset) {
		UChar *tmp;
		UErrorCode uc_err = U_ZERO_ERROR;
		int32_t r, clen, dlen;
		struct rspamd_charset_converter *conv;
		UConverter *utf8_converter;

		conv = rspamd_mime_get_converter_cached(charset, task->task_pool,
												TRUE, &uc_err);
		utf8_converter = rspamd_get_utf8_converter();

		if (conv == NULL) {
			msg_info_task("cannot open converter for %s: %s",
						  charset, u_errorName(uc_err));
			fentry->flags |= RSPAMD_ARCHIVE_FILE_OBFUSCATED;
			fentry->fname = g_string_new_len(in, inlen);

			return false;
		}

		tmp = g_malloc(sizeof(*tmp) * (inlen + 1));
		r = rspamd_converter_to_uchars(conv, tmp, inlen + 1,
									   in, inlen, &uc_err);
		if (!U_SUCCESS(uc_err)) {
			msg_info_task("cannot convert data to unicode from %s: %s",
						  charset, u_errorName(uc_err));
			g_free(tmp);

			fentry->flags |= RSPAMD_ARCHIVE_FILE_OBFUSCATED;
			fentry->fname = g_string_new_len(in, inlen);

			return NULL;
		}

		int i = 0;

		while (i < r) {
			UChar32 uc;

			U16_NEXT(tmp, i, r, uc);

			if (IS_ZERO_WIDTH_SPACE(uc) || u_iscntrl(uc)) {
				msg_info_task("control character in archive file name found: 0x%02xd "
							  "(filename=%T)",
							  uc, arch->archive_name);
				fentry->flags |= RSPAMD_ARCHIVE_FILE_OBFUSCATED;
				break;
			}
		}

		clen = ucnv_getMaxCharSize(utf8_converter);
		dlen = UCNV_GET_MAX_BYTES_FOR_STRING(r, clen);
		res = g_string_sized_new(dlen);
		r = ucnv_fromUChars(utf8_converter, res->str, dlen, tmp, r, &uc_err);

		if (!U_SUCCESS(uc_err)) {
			msg_info_task("cannot convert data from unicode from %s: %s",
						  charset, u_errorName(uc_err));
			g_free(tmp);
			g_string_free(res, TRUE);
			fentry->flags |= RSPAMD_ARCHIVE_FILE_OBFUSCATED;
			fentry->fname = g_string_new_len(in, inlen);

			return NULL;
		}

		g_free(tmp);
		res->len = r;

		msg_debug_archive("converted from %s to UTF-8 inlen: %z, outlen: %d",
						  charset, inlen, r);
		fentry->fname = res;
	}
	else {
		/* Convert unsafe characters to '?' */
		res = g_string_sized_new(inlen);
		p = in;
		end = in + inlen;

		while (p < end) {
			if (g_ascii_isgraph(*p)) {
				g_string_append_c(res, *p);
			}
			else {
				g_string_append_c(res, '?');

				if (*p < 0x7f && (g_ascii_iscntrl(*p) || *p == '\0')) {
					if (!(fentry->flags & RSPAMD_ARCHIVE_FILE_OBFUSCATED)) {
						msg_info_task("suspicious character in archive file name found: 0x%02xd "
									  "(filename=%T)",
									  (int) *p, arch->archive_name);
						fentry->flags |= RSPAMD_ARCHIVE_FILE_OBFUSCATED;
					}
				}
			}

			p++;
		}
		fentry->fname = res;
	}

	return true;
}

static void
rspamd_archive_process_zip(struct rspamd_task *task,
						   struct rspamd_mime_part *part)
{
	const unsigned char *p, *start, *end, *eocd = NULL, *cd;
	const uint32_t eocd_magic = 0x06054b50, cd_basic_len = 46;
	const unsigned char cd_magic[] = {0x50, 0x4b, 0x01, 0x02};
	const unsigned int max_processed = 1024;
	uint32_t cd_offset, cd_size, comp_size, uncomp_size, processed = 0;
	uint16_t extra_len, fname_len, comment_len;
	struct rspamd_archive *arch;
	struct rspamd_archive_file *f = NULL;

	/* Zip files have interesting data at the end of archive */
	p = part->parsed_data.begin + part->parsed_data.len - 1;
	start = part->parsed_data.begin;
	end = p;

	/* Search for EOCD:
	 * 22 bytes is a typical size of eocd without a comment and
	 * end points one byte after the last character
	 */
	p -= 21;

	while (p > start + sizeof(uint32_t)) {
		uint32_t t;

		if (processed > max_processed) {
			break;
		}

		/* XXX: not an efficient approach */
		memcpy(&t, p, sizeof(t));

		if (GUINT32_FROM_LE(t) == eocd_magic) {
			eocd = p;
			break;
		}

		p--;
		processed++;
	}


	if (eocd == NULL) {
		/* Not a zip file */
		msg_info_task("zip archive is invalid (no EOCD)");

		return;
	}

	if (end - eocd < 21) {
		msg_info_task("zip archive is invalid (short EOCD)");

		return;
	}


	memcpy(&cd_size, eocd + 12, sizeof(cd_size));
	cd_size = GUINT32_FROM_LE(cd_size);
	memcpy(&cd_offset, eocd + 16, sizeof(cd_offset));
	cd_offset = GUINT32_FROM_LE(cd_offset);

	/* We need to check sanity as well */
	if (cd_offset + cd_size > (unsigned int) (eocd - start)) {
		msg_info_task("zip archive is invalid (bad size/offset for CD)");

		return;
	}

	cd = start + cd_offset;

	arch = rspamd_mempool_alloc0(task->task_pool, sizeof(*arch));
	arch->files = g_ptr_array_new();
	arch->type = RSPAMD_ARCHIVE_ZIP;
	if (part->cd) {
		arch->archive_name = &part->cd->filename;
	}
	rspamd_mempool_add_destructor(task->task_pool, rspamd_archive_dtor,
								  arch);

	while (cd < start + cd_offset + cd_size) {
		uint16_t flags;

		/* Read central directory record */
		if (eocd - cd < cd_basic_len ||
			memcmp(cd, cd_magic, sizeof(cd_magic)) != 0) {
			msg_info_task("zip archive is invalid (bad cd record)");

			return;
		}

		memcpy(&flags, cd + 8, sizeof(uint16_t));
		flags = GUINT16_FROM_LE(flags);
		memcpy(&comp_size, cd + 20, sizeof(uint32_t));
		comp_size = GUINT32_FROM_LE(comp_size);
		memcpy(&uncomp_size, cd + 24, sizeof(uint32_t));
		uncomp_size = GUINT32_FROM_LE(uncomp_size);
		memcpy(&fname_len, cd + 28, sizeof(fname_len));
		fname_len = GUINT16_FROM_LE(fname_len);
		memcpy(&extra_len, cd + 30, sizeof(extra_len));
		extra_len = GUINT16_FROM_LE(extra_len);
		memcpy(&comment_len, cd + 32, sizeof(comment_len));
		comment_len = GUINT16_FROM_LE(comment_len);

		if (cd + fname_len + comment_len + extra_len + cd_basic_len > eocd) {
			msg_info_task("zip archive is invalid (too large cd record)");

			return;
		}

		f = g_malloc0(sizeof(*f));
		rspamd_archive_file_try_utf(task, arch, f, cd + cd_basic_len, fname_len);

		f->compressed_size = comp_size;
		f->uncompressed_size = uncomp_size;

		if (flags & 0x41u) {
			f->flags |= RSPAMD_ARCHIVE_FILE_ENCRYPTED;
		}

		if (f->fname) {
			if (f->flags & RSPAMD_ARCHIVE_FILE_OBFUSCATED) {
				arch->flags |= RSPAMD_ARCHIVE_HAS_OBFUSCATED_FILES;
			}

			g_ptr_array_add(arch->files, f);
			msg_debug_archive("found file in zip archive: %v", f->fname);
		}
		else {
			g_free(f);

			return;
		}

		/* Process extra fields */
		const unsigned char *extra = cd + fname_len + cd_basic_len;
		p = extra;

		while (p + sizeof(uint16_t) * 2 < extra + extra_len) {
			uint16_t hid, hlen;

			memcpy(&hid, p, sizeof(uint16_t));
			hid = GUINT16_FROM_LE(hid);
			memcpy(&hlen, p + sizeof(uint16_t), sizeof(uint16_t));
			hlen = GUINT16_FROM_LE(hlen);

			if (hid == 0x0017) {
				f->flags |= RSPAMD_ARCHIVE_FILE_ENCRYPTED;
			}

			p += hlen + sizeof(uint16_t) * 2;
		}

		cd += fname_len + comment_len + extra_len + cd_basic_len;
	}

	part->part_type = RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;

	arch->size = part->parsed_data.len;
}

static inline int
rspamd_archive_rar_read_vint(const unsigned char *start, gsize remain, uint64_t *res)
{
	/*
	 * From http://www.rarlab.com/technote.htm:
	 * Variable length integer. Can include one or more bytes, where
	 * lower 7 bits of every byte contain integer data and highest bit
	 * in every byte is the continuation flag.
	 * If highest bit is 0, this is the last byte in sequence.
	 * So first byte contains 7 least significant bits of integer and
	 * continuation flag. Second byte, if present, contains next 7 bits and so on.
	 */
	uint64_t t = 0;
	unsigned int shift = 0;
	const unsigned char *p = start;

	while (remain > 0 && shift <= 57) {
		if (*p & 0x80) {
			t |= ((uint64_t) (*p & 0x7f)) << shift;
		}
		else {
			t |= ((uint64_t) (*p & 0x7f)) << shift;
			p++;
			break;
		}

		shift += 7;
		p++;
		remain--;
	}

	if (remain == 0 || shift > 64) {
		return -1;
	}

	*res = GUINT64_FROM_LE(t);

	return p - start;
}

#define RAR_SKIP_BYTES(n)                                                 \
	do {                                                                  \
		if ((n) <= 0) {                                                   \
			msg_debug_archive("rar archive is invalid (bad skip value)"); \
			return;                                                       \
		}                                                                 \
		if ((gsize) (end - p) < (n)) {                                    \
			msg_debug_archive("rar archive is invalid (truncated)");      \
			return;                                                       \
		}                                                                 \
		p += (n);                                                         \
	} while (0)

#define RAR_READ_VINT()                                                    \
	do {                                                                   \
		r = rspamd_archive_rar_read_vint(p, end - p, &vint);               \
		if (r == -1) {                                                     \
			msg_debug_archive("rar archive is invalid (bad vint)");        \
			return;                                                        \
		}                                                                  \
		else if (r == 0) {                                                 \
			msg_debug_archive("rar archive is invalid (BAD vint offset)"); \
			return;                                                        \
		}                                                                  \
	} while (0)

#define RAR_READ_VINT_SKIP()                                        \
	do {                                                            \
		r = rspamd_archive_rar_read_vint(p, end - p, &vint);        \
		if (r == -1) {                                              \
			msg_debug_archive("rar archive is invalid (bad vint)"); \
			return;                                                 \
		}                                                           \
		p += r;                                                     \
	} while (0)

#define RAR_READ_UINT16(n)                                           \
	do {                                                             \
		if (end - p < (glong) sizeof(uint16_t)) {                    \
			msg_debug_archive("rar archive is invalid (bad int16)"); \
			return;                                                  \
		}                                                            \
		n = p[0] + (p[1] << 8);                                      \
		p += sizeof(uint16_t);                                       \
	} while (0)

#define RAR_READ_UINT32(n)                                                                                                \
	do {                                                                                                                  \
		if (end - p < (glong) sizeof(uint32_t)) {                                                                         \
			msg_debug_archive("rar archive is invalid (bad int32)");                                                      \
			return;                                                                                                       \
		}                                                                                                                 \
		n = (unsigned int) p[0] + ((unsigned int) p[1] << 8) + ((unsigned int) p[2] << 16) + ((unsigned int) p[3] << 24); \
		p += sizeof(uint32_t);                                                                                            \
	} while (0)

static void
rspamd_archive_process_rar_v4(struct rspamd_task *task, const unsigned char *start,
							  const unsigned char *end, struct rspamd_mime_part *part)
{
	const unsigned char *p = start, *start_section;
	uint8_t type;
	unsigned int flags;
	uint64_t sz, comp_sz = 0, uncomp_sz = 0;
	struct rspamd_archive *arch;
	struct rspamd_archive_file *f;

	arch = rspamd_mempool_alloc0(task->task_pool, sizeof(*arch));
	arch->files = g_ptr_array_new();
	arch->type = RSPAMD_ARCHIVE_RAR;
	if (part->cd) {
		arch->archive_name = &part->cd->filename;
	}
	rspamd_mempool_add_destructor(task->task_pool, rspamd_archive_dtor,
								  arch);

	while (p < end) {
		/* Crc16 */
		start_section = p;
		RAR_SKIP_BYTES(sizeof(uint16_t));
		type = *p;
		p++;
		RAR_READ_UINT16(flags);

		if (type == 0x73) {
			/* Main header, check for encryption */
			if (flags & 0x80) {
				arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
				goto end;
			}
		}

		RAR_READ_UINT16(sz);

		if (flags & 0x8000) {
			/* We also need to read ADD_SIZE element */
			uint32_t tmp;

			RAR_READ_UINT32(tmp);
			sz += tmp;
			/* This is also used as PACK_SIZE */
			comp_sz = tmp;
		}

		if (sz == 0) {
			/* Zero sized block - error */
			msg_debug_archive("rar archive is invalid (zero size block)");

			return;
		}

		if (type == 0x74) {
			unsigned int fname_len;

			/* File header */
			/* Uncompressed size */
			RAR_READ_UINT32(uncomp_sz);
			/* Skip to NAME_SIZE element */
			RAR_SKIP_BYTES(11);
			RAR_READ_UINT16(fname_len);

			if (fname_len == 0 || fname_len > (gsize) (end - p)) {
				msg_debug_archive("rar archive is invalid (bad filename size: %d)",
								  fname_len);

				return;
			}

			/* Attrs */
			RAR_SKIP_BYTES(4);

			if (flags & 0x100) {
				/* We also need to read HIGH_PACK_SIZE */
				uint32_t tmp;

				RAR_READ_UINT32(tmp);
				sz += tmp;
				comp_sz += tmp;
				/* HIGH_UNP_SIZE  */
				RAR_READ_UINT32(tmp);
				uncomp_sz += tmp;
			}

			f = g_malloc0(sizeof(*f));

			if (flags & 0x200) {
				/* We have unicode + normal version */
				unsigned char *tmp;

				tmp = memchr(p, '\0', fname_len);

				if (tmp != NULL) {
					/* Just use ASCII version */
					rspamd_archive_file_try_utf(task, arch, f, p, tmp - p);
					msg_debug_archive("found ascii filename in rarv4 archive: %v",
									  f->fname);
				}
				else {
					/* We have UTF8 filename, use it as is */
					rspamd_archive_file_try_utf(task, arch, f, p, fname_len);
					msg_debug_archive("found utf filename in rarv4 archive: %v",
									  f->fname);
				}
			}
			else {
				rspamd_archive_file_try_utf(task, arch, f, p, fname_len);
				msg_debug_archive("found ascii (old) filename in rarv4 archive: %v",
								  f->fname);
			}

			f->compressed_size = comp_sz;
			f->uncompressed_size = uncomp_sz;

			if (flags & 0x4) {
				f->flags |= RSPAMD_ARCHIVE_FILE_ENCRYPTED;
			}

			if (f->fname) {
				if (f->flags & RSPAMD_ARCHIVE_FILE_OBFUSCATED) {
					arch->flags |= RSPAMD_ARCHIVE_HAS_OBFUSCATED_FILES;
				}
				g_ptr_array_add(arch->files, f);
			}
			else {
				g_free(f);
			}
		}

		p = start_section;
		RAR_SKIP_BYTES(sz);
	}

end:
	part->part_type = RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	arch->size = part->parsed_data.len;
}

static void
rspamd_archive_process_rar(struct rspamd_task *task,
						   struct rspamd_mime_part *part)
{
	const unsigned char *p, *end, *section_start;
	const unsigned char rar_v5_magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00},
						rar_v4_magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
	const unsigned int rar_encrypted_header = 4, rar_main_header = 1,
					   rar_file_header = 2;
	uint64_t vint, sz, comp_sz = 0, uncomp_sz = 0, flags = 0, type = 0,
					   extra_sz = 0;
	struct rspamd_archive *arch;
	struct rspamd_archive_file *f;
	int r;

	p = part->parsed_data.begin;
	end = p + part->parsed_data.len;

	if ((gsize) (end - p) <= sizeof(rar_v5_magic)) {
		msg_debug_archive("rar archive is invalid (too small)");

		return;
	}

	if (memcmp(p, rar_v5_magic, sizeof(rar_v5_magic)) == 0) {
		p += sizeof(rar_v5_magic);
	}
	else if (memcmp(p, rar_v4_magic, sizeof(rar_v4_magic)) == 0) {
		p += sizeof(rar_v4_magic);

		rspamd_archive_process_rar_v4(task, p, end, part);
		return;
	}
	else {
		msg_debug_archive("rar archive is invalid (no rar magic)");

		return;
	}

	/* Rar v5 format */
	arch = rspamd_mempool_alloc0(task->task_pool, sizeof(*arch));
	arch->files = g_ptr_array_new();
	arch->type = RSPAMD_ARCHIVE_RAR;
	if (part->cd) {
		arch->archive_name = &part->cd->filename;
	}
	rspamd_mempool_add_destructor(task->task_pool, rspamd_archive_dtor,
								  arch);

	/* Now we can have either encryption header or archive header */
	/* Crc 32 */
	RAR_SKIP_BYTES(sizeof(uint32_t));
	/* Size */
	RAR_READ_VINT_SKIP();
	sz = vint;
	/* Type */
	section_start = p;
	RAR_READ_VINT_SKIP();
	type = vint;
	/* Header flags */
	RAR_READ_VINT_SKIP();
	flags = vint;

	if (flags & 0x1) {
		/* Have extra zone */
		RAR_READ_VINT_SKIP();
	}
	if (flags & 0x2) {
		/* Data zone is presented */
		RAR_READ_VINT_SKIP();
		sz += vint;
	}

	if (type == rar_encrypted_header) {
		/* We can't read any further information as archive is encrypted */
		arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
		goto end;
	}
	else if (type != rar_main_header) {
		msg_debug_archive("rar archive is invalid (bad main header)");

		return;
	}

	/* Nothing useful in main header */
	p = section_start;
	RAR_SKIP_BYTES(sz);

	while (p < end) {
		gboolean has_extra = FALSE;
		/* Read the next header */
		/* Crc 32 */
		RAR_SKIP_BYTES(sizeof(uint32_t));
		/* Size */
		RAR_READ_VINT_SKIP();

		sz = vint;
		if (sz == 0) {
			/* Zero sized block - error */
			msg_debug_archive("rar archive is invalid (zero size block)");

			return;
		}

		section_start = p;
		/* Type */
		RAR_READ_VINT_SKIP();
		type = vint;
		/* Header flags */
		RAR_READ_VINT_SKIP();
		flags = vint;

		if (flags & 0x1) {
			/* Have extra zone */
			RAR_READ_VINT_SKIP();
			extra_sz = vint;
			has_extra = TRUE;
		}

		if (flags & 0x2) {
			/* Data zone is presented */
			RAR_READ_VINT_SKIP();
			sz += vint;
			comp_sz = vint;
		}

		if (type != rar_file_header) {
			p = section_start;
			RAR_SKIP_BYTES(sz);
		}
		else {
			/* We have a file header, go forward */
			uint64_t fname_len;
			bool is_directory = false;

			/* File header specific flags */
			RAR_READ_VINT_SKIP();
			flags = vint;

			/* Unpacked size */
			RAR_READ_VINT_SKIP();
			uncomp_sz = vint;
			/* Attributes */
			RAR_READ_VINT_SKIP();

			if (flags & 0x2) {
				/* Unix mtime */
				RAR_SKIP_BYTES(sizeof(uint32_t));
			}
			if (flags & 0x4) {
				/* Crc32 */
				RAR_SKIP_BYTES(sizeof(uint32_t));
			}
			if (flags & 0x1) {
				/* Ignore directories for sanity purposes */
				is_directory = true;
				msg_debug_archive("skip directory record in a rar archive");
			}

			if (!is_directory) {
				/* Compression */
				RAR_READ_VINT_SKIP();
				/* Host OS */
				RAR_READ_VINT_SKIP();
				/* Filename length (finally!) */
				RAR_READ_VINT_SKIP();
				fname_len = vint;

				if (fname_len == 0 || fname_len > (gsize) (end - p)) {
					msg_debug_archive("rar archive is invalid (bad filename size)");

					return;
				}

				f = g_malloc0(sizeof(*f));
				f->uncompressed_size = uncomp_sz;
				f->compressed_size = comp_sz;
				rspamd_archive_file_try_utf(task, arch, f, p, fname_len);

				if (f->fname) {
					msg_debug_archive("added rarv5 file: %v", f->fname);
					g_ptr_array_add(arch->files, f);
					if (f->flags & RSPAMD_ARCHIVE_FILE_OBFUSCATED) {
						arch->flags |= RSPAMD_ARCHIVE_HAS_OBFUSCATED_FILES;
					}
				}
				else {
					g_free(f);
					f = NULL;
				}

				if (f && has_extra && extra_sz > 0 &&
					p + fname_len + extra_sz < end) {
					/* Try to find encryption record in extra field */
					const unsigned char *ex = p + fname_len;

					while (ex < p + extra_sz) {
						const unsigned char *t;
						int64_t cur_sz = 0, sec_type = 0;

						r = rspamd_archive_rar_read_vint(ex, extra_sz, &cur_sz);
						if (r == -1) {
							msg_debug_archive("rar archive is invalid (bad vint)");
							return;
						}

						t = ex + r;

						r = rspamd_archive_rar_read_vint(t, extra_sz - r, &sec_type);
						if (r == -1) {
							msg_debug_archive("rar archive is invalid (bad vint)");
							return;
						}

						if (sec_type == 0x01) {
							f->flags |= RSPAMD_ARCHIVE_FILE_ENCRYPTED;
							arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
							break;
						}

						ex += cur_sz;
					}
				}
			}

			/* Restore p to the beginning of the header */
			p = section_start;
			RAR_SKIP_BYTES(sz);
		}
	}

end:
	part->part_type = RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	arch->size = part->parsed_data.len;
}

static inline int
rspamd_archive_7zip_read_vint(const unsigned char *start, gsize remain, uint64_t *res)
{
	/*
	 * REAL_UINT64 means real UINT64.
	 * UINT64 means real UINT64 encoded with the following scheme:
	 *
	 * Size of encoding sequence depends from first byte:
	 * First_Byte  Extra_Bytes        Value
	 * (binary)
	 * 0xxxxxxx               : ( xxxxxxx           )
	 * 10xxxxxx    BYTE y[1]  : (  xxxxxx << (8 * 1)) + y
	 * 110xxxxx    BYTE y[2]  : (   xxxxx << (8 * 2)) + y
	 * ...
	 * 1111110x    BYTE y[6]  : (       x << (8 * 6)) + y
	 * 11111110    BYTE y[7]  :                         y
	 * 11111111    BYTE y[8]  :                         y
	 */
	unsigned char t;

	if (remain == 0) {
		return -1;
	}

	t = *start;

	if (!isset(&t, 7)) {
		/* Trivial case */
		*res = t;
		return 1;
	}
	else if (t == 0xFF) {
		if (remain >= sizeof(uint64_t) + 1) {
			memcpy(res, start + 1, sizeof(uint64_t));
			*res = GUINT64_FROM_LE(*res);

			return sizeof(uint64_t) + 1;
		}
	}
	else {
		int cur_bit = 6, intlen = 1;
		const unsigned char bmask = 0xFF;
		uint64_t tgt;

		while (cur_bit > 0) {
			if (!isset(&t, cur_bit)) {
				if (remain >= intlen + 1) {
					memcpy(&tgt, start + 1, intlen);
					tgt = GUINT64_FROM_LE(tgt);
					/* Shift back */
					tgt >>= sizeof(tgt) - NBBY * intlen;
					/* Add masked value */
					tgt += (uint64_t) (t & (bmask >> (NBBY - cur_bit)))
						   << (NBBY * intlen);
					*res = tgt;

					return intlen + 1;
				}
			}
			cur_bit--;
			intlen++;
		}
	}

	return -1;
}

#define SZ_READ_VINT_SKIP()                                        \
	do {                                                           \
		r = rspamd_archive_7zip_read_vint(p, end - p, &vint);      \
		if (r == -1) {                                             \
			msg_debug_archive("7z archive is invalid (bad vint)"); \
			return;                                                \
		}                                                          \
		p += r;                                                    \
	} while (0)
#define SZ_READ_VINT(var)                                                        \
	do {                                                                         \
		int r;                                                                   \
		r = rspamd_archive_7zip_read_vint(p, end - p, &(var));                   \
		if (r == -1) {                                                           \
			msg_debug_archive("7z archive is invalid (bad vint): %s", G_STRLOC); \
			return NULL;                                                         \
		}                                                                        \
		p += r;                                                                  \
	} while (0)

#define SZ_READ_UINT64(n)                                                            \
	do {                                                                             \
		if (end - p < (goffset) sizeof(uint64_t)) {                                  \
			msg_debug_archive("7zip archive is invalid (bad uint64): %s", G_STRLOC); \
			return;                                                                  \
		}                                                                            \
		memcpy(&(n), p, sizeof(uint64_t));                                           \
		n = GUINT64_FROM_LE(n);                                                      \
		p += sizeof(uint64_t);                                                       \
	} while (0)
#define SZ_SKIP_BYTES(n)                                                                                                                           \
	do {                                                                                                                                           \
		if (end - p >= (n)) {                                                                                                                      \
			p += (n);                                                                                                                              \
		}                                                                                                                                          \
		else {                                                                                                                                     \
			msg_debug_archive("7zip archive is invalid (truncated); wanted to read %d bytes, %d avail: %s", (int) (n), (int) (end - p), G_STRLOC); \
			return NULL;                                                                                                                           \
		}                                                                                                                                          \
	} while (0)

enum rspamd_7zip_header_mark {
	kEnd = 0x00,
	kHeader = 0x01,
	kArchiveProperties = 0x02,
	kAdditionalStreamsInfo = 0x03,
	kMainStreamsInfo = 0x04,
	kFilesInfo = 0x05,
	kPackInfo = 0x06,
	kUnPackInfo = 0x07,
	kSubStreamsInfo = 0x08,
	kSize = 0x09,
	kCRC = 0x0A,
	kFolder = 0x0B,
	kCodersUnPackSize = 0x0C,
	kNumUnPackStream = 0x0D,
	kEmptyStream = 0x0E,
	kEmptyFile = 0x0F,
	kAnti = 0x10,
	kName = 0x11,
	kCTime = 0x12,
	kATime = 0x13,
	kMTime = 0x14,
	kWinAttributes = 0x15,
	kComment = 0x16,
	kEncodedHeader = 0x17,
	kStartPos = 0x18,
	kDummy = 0x19,
};


#define _7Z_CRYPTO_MAIN_ZIP 0x06F10101        /* Main Zip crypto algo */
#define _7Z_CRYPTO_RAR_29 0x06F10303          /* Rar29 AES-128 + (modified SHA-1) */
#define _7Z_CRYPTO_AES_256_SHA_256 0x06F10701 /* AES-256 + SHA-256 */

#define IS_SZ_ENCRYPTED(codec_id) (((codec_id) == _7Z_CRYPTO_MAIN_ZIP) || \
								   ((codec_id) == _7Z_CRYPTO_RAR_29) ||   \
								   ((codec_id) == _7Z_CRYPTO_AES_256_SHA_256))

static const unsigned char *
rspamd_7zip_read_bits(struct rspamd_task *task,
					  const unsigned char *p, const unsigned char *end,
					  struct rspamd_archive *arch, unsigned int nbits,
					  unsigned int *pbits_set)
{
	unsigned mask = 0, avail = 0, i;
	gboolean bit_set = 0;

	for (i = 0; i < nbits; i++) {
		if (mask == 0) {
			avail = *p;
			SZ_SKIP_BYTES(1);
			mask = 0x80;
		}

		bit_set = (avail & mask) ? 1 : 0;

		if (bit_set && pbits_set) {
			(*pbits_set)++;
		}

		mask >>= 1;
	}

	return p;
}

static const unsigned char *
rspamd_7zip_read_digest(struct rspamd_task *task,
						const unsigned char *p, const unsigned char *end,
						struct rspamd_archive *arch,
						uint64_t num_streams,
						unsigned int *pdigest_read)
{
	unsigned char all_defined = *p;
	uint64_t i;
	unsigned int num_defined = 0;
	/*
	 * BYTE AllAreDefined
	 *  if (AllAreDefined == 0)
	 *  {
	 *    for(NumStreams)
	 *    BIT Defined
	 *  }
	 *  UINT32 CRCs[NumDefined]
	 */
	SZ_SKIP_BYTES(1);

	if (all_defined) {
		num_defined = num_streams;
	}
	else {
		if (num_streams > 8192) {
			/* Gah */
			return NULL;
		}

		p = rspamd_7zip_read_bits(task, p, end, arch, num_streams, &num_defined);

		if (p == NULL) {
			return NULL;
		}
	}

	for (i = 0; i < num_defined; i++) {
		SZ_SKIP_BYTES(sizeof(uint32_t));
	}

	if (pdigest_read) {
		*pdigest_read = num_defined;
	}

	return p;
}

static const unsigned char *
rspamd_7zip_read_pack_info(struct rspamd_task *task,
						   const unsigned char *p, const unsigned char *end,
						   struct rspamd_archive *arch)
{
	uint64_t pack_pos = 0, pack_streams = 0, i, cur_sz;
	unsigned int num_digests = 0;
	unsigned char t;
	/*
	 *  UINT64 PackPos
	 *  UINT64 NumPackStreams
	 *
	 *  []
	 *  BYTE NID::kSize    (0x09)
	 *  UINT64 PackSizes[NumPackStreams]
	 *  []
	 *
	 *  []
	 *  BYTE NID::kCRC      (0x0A)
	 *  PackStreamDigests[NumPackStreams]
	 *  []
	 *  BYTE NID::kEnd
	 */

	SZ_READ_VINT(pack_pos);
	SZ_READ_VINT(pack_streams);

	while (p != NULL && p < end) {
		t = *p;
		SZ_SKIP_BYTES(1);
		msg_debug_archive("7zip: read pack info %xc", t);

		switch (t) {
		case kSize:
			/* We need to skip pack_streams VINTS */
			for (i = 0; i < pack_streams; i++) {
				SZ_READ_VINT(cur_sz);
			}
			break;
		case kCRC:
			/* CRCs are more complicated */
			p = rspamd_7zip_read_digest(task, p, end, arch, pack_streams,
										&num_digests);
			break;
		case kEnd:
			goto end;
			break;
		default:
			p = NULL;
			msg_debug_archive("bad 7zip type: %xc; %s", t, G_STRLOC);
			goto end;
			break;
		}
	}

end:

	return p;
}

static const unsigned char *
rspamd_7zip_read_folder(struct rspamd_task *task,
						const unsigned char *p, const unsigned char *end,
						struct rspamd_archive *arch, unsigned int *pnstreams, unsigned int *ndigests)
{
	uint64_t ncoders = 0, i, j, noutstreams = 0, ninstreams = 0;

	SZ_READ_VINT(ncoders);

	for (i = 0; i < ncoders && p != NULL && p < end; i++) {
		uint64_t sz, tmp;
		unsigned char t;
		/*
		 * BYTE
		 * {
		 *   0:3 CodecIdSize
		 *   4:  Is Complex Coder
		 *   5:  There Are Attributes
		 *   6:  Reserved
		 *   7:  There are more alternative methods. (Not used anymore, must be 0).
		 * }
		 * BYTE CodecId[CodecIdSize]
		 * if (Is Complex Coder)
		 * {
		 *   UINT64 NumInStreams;
		 *   UINT64 NumOutStreams;
		 * }
		 * if (There Are Attributes)
		 * {
		 *   UINT64 PropertiesSize
		 *   BYTE Properties[PropertiesSize]
		 * }
		 */
		t = *p;
		SZ_SKIP_BYTES(1);
		sz = t & 0xF;
		/* Codec ID */
		tmp = 0;
		for (j = 0; j < sz; j++) {
			tmp <<= 8;
			tmp += p[j];
		}

		msg_debug_archive("7zip: read codec id: %L", tmp);

		if (IS_SZ_ENCRYPTED(tmp)) {
			msg_debug_archive("7zip: encrypted codec: %L", tmp);
			arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
		}

		SZ_SKIP_BYTES(sz);

		if (t & (1u << 4)) {
			/* Complex */
			SZ_READ_VINT(tmp); /* InStreams */
			ninstreams += tmp;
			SZ_READ_VINT(tmp); /* OutStreams */
			noutstreams += tmp;
		}
		else {
			/* XXX: is it correct ? */
			noutstreams++;
			ninstreams++;
		}
		if (t & (1u << 5)) {
			/* Attributes ... */
			SZ_READ_VINT(tmp); /* Size of attrs */
			SZ_SKIP_BYTES(tmp);
		}
	}

	if (noutstreams > 1) {
		/* BindPairs, WTF, huh */
		for (i = 0; i < noutstreams - 1; i++) {
			uint64_t tmp;

			SZ_READ_VINT(tmp);
			SZ_READ_VINT(tmp);
		}
	}

	int64_t npacked = (int64_t) ninstreams - (int64_t) noutstreams + 1;
	msg_debug_archive("7zip: instreams=%L, outstreams=%L, packed=%L",
					  ninstreams, noutstreams, npacked);

	if (npacked > 1) {
		/* Gah... */
		for (i = 0; i < npacked; i++) {
			uint64_t tmp;

			SZ_READ_VINT(tmp);
		}
	}

	*pnstreams = noutstreams;
	(*ndigests) += npacked;

	return p;
}

static const unsigned char *
rspamd_7zip_read_coders_info(struct rspamd_task *task,
							 const unsigned char *p, const unsigned char *end,
							 struct rspamd_archive *arch,
							 unsigned int *pnum_folders, unsigned int *pnum_nodigest)
{
	uint64_t num_folders = 0, i, tmp;
	unsigned char t;
	unsigned int *folder_nstreams = NULL, num_digests = 0, digests_read = 0;

	while (p != NULL && p < end) {
		/*
		 * BYTE NID::kFolder  (0x0B)
		 *  UINT64 NumFolders
		 *  BYTE External
		 *  switch(External)
		 *  {
		 * 	case 0:
		 * 	  Folders[NumFolders]
		 * 	case 1:
		 * 	  UINT64 DataStreamIndex
		 *   }
		 *   BYTE ID::kCodersUnPackSize  (0x0C)
		 *   for(Folders)
		 * 	for(Folder.NumOutStreams)
		 * 	 UINT64 UnPackSize;
		 *   []
		 *   BYTE NID::kCRC   (0x0A)
		 *   UnPackDigests[NumFolders]
		 *   []
		 *   BYTE NID::kEnd
		 */

		t = *p;
		SZ_SKIP_BYTES(1);
		msg_debug_archive("7zip: read coders info %xc", t);

		switch (t) {
		case kFolder:
			SZ_READ_VINT(num_folders);
			msg_debug_archive("7zip: nfolders=%L", num_folders);

			if (*p != 0) {
				/* External folders */
				SZ_SKIP_BYTES(1);
				SZ_READ_VINT(tmp);
			}
			else {
				SZ_SKIP_BYTES(1);

				if (num_folders > 8192) {
					/* Gah */
					return NULL;
				}

				if (folder_nstreams) {
					g_free(folder_nstreams);
				}

				folder_nstreams = g_malloc(sizeof(int) * num_folders);

				for (i = 0; i < num_folders && p != NULL && p < end; i++) {
					p = rspamd_7zip_read_folder(task, p, end, arch,
												&folder_nstreams[i], &num_digests);
				}
			}
			break;
		case kCodersUnPackSize:
			for (i = 0; i < num_folders && p != NULL && p < end; i++) {
				if (folder_nstreams) {
					for (unsigned int j = 0; j < folder_nstreams[i]; j++) {
						SZ_READ_VINT(tmp); /* Unpacked size */
						msg_debug_archive("7zip: unpacked size "
										  "(folder=%d, stream=%d) = %L",
										  (int) i, j, tmp);
					}
				}
				else {
					msg_err_task("internal 7zip error");
				}
			}
			break;
		case kCRC:
			/*
			 * Here are dragons. Spec tells that here there could be up
			 * to nfolders digests. However, according to the actual source
			 * code, in case of multiple out streams there should be digests
			 * for all out streams.
			 *
			 * In the real life (tm) it is even more idiotic: all these digests
			 * are in another section! But that section needs number of digests
			 * that are absent here. It is the most stupid thing I've ever seen
			 * in any file format.
			 *
			 * I hope there *WAS* some reason to do such shit...
			 */
			p = rspamd_7zip_read_digest(task, p, end, arch, num_digests,
										&digests_read);
			break;
		case kEnd:
			goto end;
			break;
		default:
			p = NULL;
			msg_debug_archive("bad 7zip type: %xc; %s", t, G_STRLOC);
			goto end;
			break;
		}
	}

end:

	if (pnum_nodigest) {
		*pnum_nodigest = num_digests - digests_read;
	}
	if (pnum_folders) {
		*pnum_folders = num_folders;
	}

	if (folder_nstreams) {
		g_free(folder_nstreams);
	}

	return p;
}

static const unsigned char *
rspamd_7zip_read_substreams_info(struct rspamd_task *task,
								 const unsigned char *p, const unsigned char *end,
								 struct rspamd_archive *arch,
								 unsigned int num_folders, unsigned int num_nodigest)
{
	unsigned char t;
	unsigned int i;
	uint64_t *folder_nstreams;

	if (num_folders > 8192) {
		/* Gah */
		return NULL;
	}

	folder_nstreams = g_alloca(sizeof(uint64_t) * num_folders);
	memset(folder_nstreams, 0, sizeof(uint64_t) * num_folders);

	while (p != NULL && p < end) {
		/*
		 * []
		 *  BYTE NID::kNumUnPackStream; (0x0D)
		 *  UINT64 NumUnPackStreamsInFolders[NumFolders];
		 *  []
		 *
		 *  []
		 *  BYTE NID::kSize  (0x09)
		 *  UINT64 UnPackSizes[??]
		 *  []
		 *
		 *
		 *  []
		 *  BYTE NID::kCRC  (0x0A)
		 *  Digests[Number of streams with unknown CRC]
		 *  []

		 */
		t = *p;
		SZ_SKIP_BYTES(1);

		msg_debug_archive("7zip: read substream info %xc", t);

		switch (t) {
		case kNumUnPackStream:
			for (i = 0; i < num_folders; i++) {
				uint64_t tmp;

				SZ_READ_VINT(tmp);
				folder_nstreams[i] = tmp;
			}
			break;
		case kCRC:
			/*
			 * Read the comment in the rspamd_7zip_read_coders_info
			 */
			p = rspamd_7zip_read_digest(task, p, end, arch, num_nodigest,
										NULL);
			break;
		case kSize:
			/*
			 * Another brain damaged logic, but we have to support it
			 * as there are no ways to proceed without it.
			 * In fact, it is just absent in the real life...
			 */
			for (i = 0; i < num_folders; i++) {
				for (unsigned int j = 0; j < folder_nstreams[i]; j++) {
					uint64_t tmp;

					SZ_READ_VINT(tmp); /* Who cares indeed */
				}
			}
			break;
		case kEnd:
			goto end;
			break;
		default:
			p = NULL;
			msg_debug_archive("bad 7zip type: %xc; %s", t, G_STRLOC);
			goto end;
			break;
		}
	}

end:
	return p;
}

static const unsigned char *
rspamd_7zip_read_main_streams_info(struct rspamd_task *task,
								   const unsigned char *p, const unsigned char *end,
								   struct rspamd_archive *arch)
{
	unsigned char t;
	unsigned int num_folders = 0, unknown_digests = 0;

	while (p != NULL && p < end) {
		t = *p;
		SZ_SKIP_BYTES(1);
		msg_debug_archive("7zip: read main streams info %xc", t);

		/*
		 *
		 *  []
		 *  PackInfo
		 *  []

		 *  []
		 *  CodersInfo
		 *  []
		 *
		 *  []
		 *  SubStreamsInfo
		 *  []
		 *
		 *  BYTE NID::kEnd
		 */
		switch (t) {
		case kPackInfo:
			p = rspamd_7zip_read_pack_info(task, p, end, arch);
			break;
		case kUnPackInfo:
			p = rspamd_7zip_read_coders_info(task, p, end, arch, &num_folders,
											 &unknown_digests);
			break;
		case kSubStreamsInfo:
			p = rspamd_7zip_read_substreams_info(task, p, end, arch, num_folders,
												 unknown_digests);
			break;
			break;
		case kEnd:
			goto end;
			break;
		default:
			p = NULL;
			msg_debug_archive("bad 7zip type: %xc; %s", t, G_STRLOC);
			goto end;
			break;
		}
	}

end:
	return p;
}

static const unsigned char *
rspamd_7zip_read_archive_props(struct rspamd_task *task,
							   const unsigned char *p, const unsigned char *end,
							   struct rspamd_archive *arch)
{
	unsigned char proptype;
	uint64_t proplen;

	/*
	 * for (;;)
	 * {
	 *   BYTE PropertyType;
	 *   if (aType == 0)
	 *     break;
	 *   UINT64 PropertySize;
	 *   BYTE PropertyData[PropertySize];
	 * }
	 */

	if (p != NULL) {
		proptype = *p;
		SZ_SKIP_BYTES(1);

		while (proptype != 0) {
			SZ_READ_VINT(proplen);

			if (p + proplen < end) {
				p += proplen;
			}
			else {
				return NULL;
			}

			proptype = *p;
			SZ_SKIP_BYTES(1);
		}
	}

	return p;
}

static GString *
rspamd_7zip_ucs2_to_utf8(struct rspamd_task *task, const unsigned char *p,
						 const unsigned char *end)
{
	GString *res;
	goffset dest_pos = 0, src_pos = 0;
	const gsize len = (end - p) / sizeof(uint16_t);
	uint16_t *up;
	UChar32 wc;
	UBool is_error = 0;

	res = g_string_sized_new((end - p) * 3 / 2 + sizeof(wc) + 1);
	up = (uint16_t *) p;

	while (src_pos < len) {
		U16_NEXT(up, src_pos, len, wc);

		if (wc > 0) {
			U8_APPEND(res->str, dest_pos,
					  res->allocated_len - 1,
					  wc, is_error);
		}

		if (is_error) {
			g_string_free(res, TRUE);

			return NULL;
		}
	}

	g_assert(dest_pos < res->allocated_len);

	res->len = dest_pos;
	res->str[dest_pos] = '\0';

	return res;
}

static const unsigned char *
rspamd_7zip_read_files_info(struct rspamd_task *task,
							const unsigned char *p, const unsigned char *end,
							struct rspamd_archive *arch)
{
	uint64_t nfiles = 0, sz, i;
	unsigned char t, b;
	struct rspamd_archive_file *fentry;

	SZ_READ_VINT(nfiles);

	for (; p != NULL && p < end;) {
		t = *p;
		SZ_SKIP_BYTES(1);

		msg_debug_archive("7zip: read file data type %xc", t);

		if (t == kEnd) {
			goto end;
		}

		/* This is SO SPECIAL, gah */
		SZ_READ_VINT(sz);

		switch (t) {
		case kEmptyStream:
		case kEmptyFile:
		case kAnti: /* AntiFile, OMFG */
					/* We don't care about these bits */
		case kCTime:
		case kATime:
		case kMTime:
			/* We don't care of these guys, but we still have to parse them, gah */
			if (sz > 0) {
				SZ_SKIP_BYTES(sz);
			}
			break;
		case kName:
			/* The most useful part in this whole bloody format */
			b = *p; /* External flag */
			SZ_SKIP_BYTES(1);

			if (b) {
				/* TODO: for the god sake, do something about external
				 * filenames...
				 */
				uint64_t tmp;

				SZ_READ_VINT(tmp);
			}
			else {
				for (i = 0; i < nfiles; i++) {
					/* Zero terminated wchar_t: happy converting... */
					/* First, find terminator */
					const unsigned char *fend = NULL, *tp = p;
					GString *res;

					while (tp < end - 1) {
						if (*tp == 0 && *(tp + 1) == 0) {
							fend = tp;
							break;
						}

						tp += 2;
					}

					if (fend == NULL || fend - p == 0) {
						/* Crap instead of fname */
						msg_debug_archive("bad 7zip name; %s", G_STRLOC);
						goto end;
					}

					res = rspamd_7zip_ucs2_to_utf8(task, p, fend);

					if (res != NULL) {
						fentry = g_malloc0(sizeof(*fentry));
						fentry->fname = res;
						g_ptr_array_add(arch->files, fentry);
						msg_debug_archive("7zip: found file %v", res);
					}
					else {
						msg_debug_archive("bad 7zip name; %s", G_STRLOC);
					}
					/* Skip zero terminating character */
					p = fend + 2;
				}
			}
			break;
		case kDummy:
		case kWinAttributes:
			if (sz > 0) {
				SZ_SKIP_BYTES(sz);
			}
			break;
		default:
			p = NULL;
			msg_debug_archive("bad 7zip type: %xc; %s", t, G_STRLOC);
			goto end;
			break;
		}
	}

end:
	return p;
}

static const unsigned char *
rspamd_7zip_read_next_section(struct rspamd_task *task,
							  const unsigned char *p, const unsigned char *end,
							  struct rspamd_archive *arch,
							  struct rspamd_mime_part *part)
{
	unsigned char t = *p;

	SZ_SKIP_BYTES(1);

	msg_debug_archive("7zip: read section %xc", t);

	switch (t) {
	case kHeader:
		/* We just skip byte and go further */
		break;
	case kEncodedHeader:
		/*
		 * In fact, headers are just packed, but we assume it as
		 * encrypted to distinguish from the normal archives
		 */
		{
			msg_debug_archive("7zip: encoded header, needs to be uncompressed");
			struct archive *a = archive_read_new();
			archive_read_support_format_7zip(a);
			int r = archive_read_open_memory(a, part->parsed_data.begin, part->parsed_data.len);
			if (r != ARCHIVE_OK) {
				msg_debug_archive("7zip: cannot open memory archive: %s", archive_error_string(a));
				archive_read_free(a);
				return NULL;
			}

			/* Clean the existing files if any */
			rspamd_archive_dtor(arch);
			arch->files = g_ptr_array_new();

			struct archive_entry *ae;

			while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
				const char *name = archive_entry_pathname_utf8(ae);
				if (name) {
					msg_debug_archive("7zip: found file %s", name);
					struct rspamd_archive_file *f = g_malloc0(sizeof(*f));
					f->fname = g_string_new(name);
					g_ptr_array_add(arch->files, f);
				}
				archive_read_data_skip(a);
			}

			if (archive_read_has_encrypted_entries(a) > 0) {
				msg_debug_archive("7zip: found encrypted stuff");
				arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
			}

			archive_read_free(a);
			p = NULL; /* Stop internal processor, as we rely on libarchive here */
			break;
		}
	case kArchiveProperties:
		p = rspamd_7zip_read_archive_props(task, p, end, arch);
		break;
	case kMainStreamsInfo:
		p = rspamd_7zip_read_main_streams_info(task, p, end, arch);
		break;
	case kAdditionalStreamsInfo:
		p = rspamd_7zip_read_main_streams_info(task, p, end, arch);
		break;
	case kFilesInfo:
		p = rspamd_7zip_read_files_info(task, p, end, arch);
		break;
	case kEnd:
		p = NULL;
		msg_debug_archive("7zip: read final section");
		break;
	default:
		p = NULL;
		msg_debug_archive("bad 7zip type: %xc; %s", t, G_STRLOC);
		break;
	}

	return p;
}

static void
rspamd_archive_process_7zip(struct rspamd_task *task,
							struct rspamd_mime_part *part)
{
	struct rspamd_archive *arch;
	const unsigned char *start, *p, *end;
	const unsigned char sz_magic[] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
	uint64_t section_offset = 0, section_length = 0;

	start = part->parsed_data.begin;
	p = start;
	end = p + part->parsed_data.len;

	if (end - p <= sizeof(uint64_t) + sizeof(uint32_t) ||
		memcmp(p, sz_magic, sizeof(sz_magic)) != 0) {
		msg_debug_archive("7z archive is invalid (no 7z magic)");

		return;
	}

	arch = rspamd_mempool_alloc0(task->task_pool, sizeof(*arch));
	arch->files = g_ptr_array_new();
	arch->type = RSPAMD_ARCHIVE_7ZIP;
	rspamd_mempool_add_destructor(task->task_pool, rspamd_archive_dtor,
								  arch);

	/* Magic (6 bytes) + version (2 bytes) + crc32 (4 bytes) */
	p += sizeof(uint64_t) + sizeof(uint32_t);

	SZ_READ_UINT64(section_offset);
	SZ_READ_UINT64(section_length);

	if (end - p > sizeof(uint32_t)) {
		p += sizeof(uint32_t);
	}
	else {
		msg_debug_archive("7z archive is invalid (truncated crc)");

		return;
	}

	if (end - p > section_offset) {
		p += section_offset;
	}
	else {
		msg_debug_archive("7z archive is invalid (incorrect section offset)");

		return;
	}

	while ((p = rspamd_7zip_read_next_section(task, p, end, arch, part)) != NULL);

	part->part_type = RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	if (part->cd != NULL) {
		arch->archive_name = &part->cd->filename;
	}
	arch->size = part->parsed_data.len;
}

static void
rspamd_archive_process_gzip(struct rspamd_task *task,
							struct rspamd_mime_part *part)
{
	struct rspamd_archive *arch;
	const unsigned char *start, *p, *end;
	const unsigned char gz_magic[] = {0x1F, 0x8B};
	unsigned char flags;

	start = part->parsed_data.begin;
	p = start;
	end = p + part->parsed_data.len;

	if (end - p <= 10 || memcmp(p, gz_magic, sizeof(gz_magic)) != 0) {
		msg_debug_archive("gzip archive is invalid (no gzip magic)");

		return;
	}

	arch = rspamd_mempool_alloc0(task->task_pool, sizeof(*arch));
	arch->files = g_ptr_array_sized_new(1);
	arch->type = RSPAMD_ARCHIVE_GZIP;
	if (part->cd) {
		arch->archive_name = &part->cd->filename;
	}
	rspamd_mempool_add_destructor(task->task_pool, rspamd_archive_dtor,
								  arch);

	flags = p[3];

	if (flags & (1u << 5)) {
		arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
	}

	if (flags & (1u << 3)) {
		/* We have file name presented in archive, try to use it */
		if (flags & (1u << 1)) {
			/* Multipart */
			p += 12;
		}
		else {
			p += 10;
		}

		if (flags & (1u << 2)) {
			/* Optional section */
			uint16_t optlen = 0;

			RAR_READ_UINT16(optlen);

			if (end <= p + optlen) {
				msg_debug_archive("gzip archive is invalid, bad extra length: %d",
								  (int) optlen);

				return;
			}

			p += optlen;
		}

		/* Read file name */
		const unsigned char *fname_start = p;

		while (p < end) {
			if (*p == '\0') {
				if (p > fname_start) {
					struct rspamd_archive_file *f;

					f = g_malloc0(sizeof(*f));

					rspamd_archive_file_try_utf(task, arch, f,
												fname_start, p - fname_start);

					if (f->fname) {
						g_ptr_array_add(arch->files, f);

						if (f->flags & RSPAMD_ARCHIVE_FILE_OBFUSCATED) {
							arch->flags |= RSPAMD_ARCHIVE_HAS_OBFUSCATED_FILES;
						}
					}
					else {
						/* Invalid filename, skip */
						g_free(f);
					}

					goto set;
				}
			}

			p++;
		}

		/* Wrong filename, not zero terminated */
		msg_debug_archive("gzip archive is invalid, bad filename at pos %d",
						  (int) (p - start));

		return;
	}

	/* Fallback, we need to extract file name from archive name if possible */
	if (part->cd && part->cd->filename.len > 0) {
		const char *dot_pos, *slash_pos;

		dot_pos = rspamd_memrchr(part->cd->filename.begin, '.',
								 part->cd->filename.len);

		if (dot_pos) {
			struct rspamd_archive_file *f;

			slash_pos = rspamd_memrchr(part->cd->filename.begin, '/',
									   part->cd->filename.len);

			if (slash_pos && slash_pos < dot_pos) {
				f = g_malloc0(sizeof(*f));
				f->fname = g_string_sized_new(dot_pos - slash_pos);
				g_string_append_len(f->fname, slash_pos + 1,
									dot_pos - slash_pos - 1);

				msg_debug_archive("fallback to gzip filename based on cd: %v",
								  f->fname);

				g_ptr_array_add(arch->files, f);

				goto set;
			}
			else {
				const char *fname_start = part->cd->filename.begin;

				f = g_malloc0(sizeof(*f));

				if (memchr(fname_start, '.', part->cd->filename.len) != dot_pos) {
					/* Double dots, something like foo.exe.gz */
					f->fname = g_string_sized_new(dot_pos - fname_start);
					g_string_append_len(f->fname, fname_start,
										dot_pos - fname_start);
				}
				else {
					/* Single dot, something like foo.gzz */
					f->fname = g_string_sized_new(part->cd->filename.len);
					g_string_append_len(f->fname, fname_start,
										part->cd->filename.len);
				}

				msg_debug_archive("fallback to gzip filename based on cd: %v",
								  f->fname);

				g_ptr_array_add(arch->files, f);

				goto set;
			}
		}
	}

	return;

set:
	/* Set archive data */
	part->part_type = RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	arch->size = part->parsed_data.len;
}

static gboolean
rspamd_archive_cheat_detect(struct rspamd_mime_part *part, const char *str,
							const unsigned char *magic_start, gsize magic_len)
{
	struct rspamd_content_type *ct;
	const char *p;
	rspamd_ftok_t srch, *fname;

	ct = part->ct;
	RSPAMD_FTOK_ASSIGN(&srch, "application");

	if (ct && ct->type.len && ct->subtype.len > 0 && rspamd_ftok_cmp(&ct->type, &srch) == 0) {
		if (rspamd_substring_search_caseless(ct->subtype.begin, ct->subtype.len,
											 str, strlen(str)) != -1) {
			/* We still need to check magic, see #1848 */
			if (magic_start != NULL) {
				if (part->parsed_data.len > magic_len &&
					memcmp(part->parsed_data.begin,
						   magic_start, magic_len) == 0) {
					return TRUE;
				}
				/* No magic, refuse this type of archive */
				return FALSE;
			}
			else {
				return TRUE;
			}
		}
	}

	if (part->cd) {
		fname = &part->cd->filename;

		if (fname && fname->len > strlen(str)) {
			p = fname->begin + fname->len - strlen(str);

			if (rspamd_lc_cmp(p, str, strlen(str)) == 0) {
				if (*(p - 1) == '.') {
					if (magic_start != NULL) {
						if (part->parsed_data.len > magic_len &&
							memcmp(part->parsed_data.begin,
								   magic_start, magic_len) == 0) {
							return TRUE;
						}
						/* No magic, refuse this type of archive */
						return FALSE;
					}

					return TRUE;
				}
			}
		}

		if (magic_start != NULL) {
			if (part->parsed_data.len > magic_len &&
				memcmp(part->parsed_data.begin, magic_start, magic_len) == 0) {
				return TRUE;
			}
		}
	}
	else {
		if (magic_start != NULL) {
			if (part->parsed_data.len > magic_len &&
				memcmp(part->parsed_data.begin, magic_start, magic_len) == 0) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

void rspamd_archives_process(struct rspamd_task *task)
{
	unsigned int i;
	struct rspamd_mime_part *part;

	PTR_ARRAY_FOREACH(MESSAGE_FIELD(task, parts), i, part)
	{
		if (part->parsed_data.len > 0 && part->part_type != RSPAMD_MIME_PART_ARCHIVE) {
			const char *ext = part->detected_ext;
			if (ext) {
				if (g_ascii_strcasecmp(ext, "zip") == 0) {
					rspamd_archive_process_zip(task, part);
				}
				else if (g_ascii_strcasecmp(ext, "rar") == 0) {
					rspamd_archive_process_rar(task, part);
				}
				else if (g_ascii_strcasecmp(ext, "7z") == 0) {
					rspamd_archive_process_7zip(task, part);
				}
				else if (g_ascii_strcasecmp(ext, "gz") == 0) {
					rspamd_archive_process_gzip(task, part);
				}
			}

			if (part->ct && (part->ct->flags & RSPAMD_CONTENT_TYPE_TEXT) &&
				part->part_type == RSPAMD_MIME_PART_ARCHIVE &&
				part->specific.arch) {
				struct rspamd_archive *arch = part->specific.arch;

				msg_info_task("found %s archive with incorrect content-type: %T/%T",
							  rspamd_archive_type_str(arch->type),
							  &part->ct->type, &part->ct->subtype);

				if (!(part->ct->flags & RSPAMD_CONTENT_TYPE_MISSING)) {
					part->ct->flags |= RSPAMD_CONTENT_TYPE_BROKEN;
				}
			}
		}
	}
}


const char *
rspamd_archive_type_str(enum rspamd_archive_type type)
{
	const char *ret = "unknown";

	switch (type) {
	case RSPAMD_ARCHIVE_ZIP:
		ret = "zip";
		break;
	case RSPAMD_ARCHIVE_RAR:
		ret = "rar";
		break;
	case RSPAMD_ARCHIVE_7ZIP:
		ret = "7z";
		break;
	case RSPAMD_ARCHIVE_GZIP:
		ret = "gz";
		break;
	}

	return ret;
}
