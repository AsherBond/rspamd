/*
 * Copyright 2025 Vsevolod Stakhov
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
#ifndef RSPAMD_LOGGER_PRIVATE_H
#define RSPAMD_LOGGER_PRIVATE_H

#include "logger.h"

/* How much message should be repeated before it is count to be repeated one */
#define REPEATS_MIN 3
#define REPEATS_MAX 300
#define LOGBUF_LEN 8192

enum rspamd_log_tag_strip_policy {
	RSPAMD_LOG_TAG_STRIP_RIGHT = 0, /* Cut right part (current behavior) */
	RSPAMD_LOG_TAG_STRIP_LEFT,      /* Cut left part (take last elements) */
	RSPAMD_LOG_TAG_STRIP_MIDDLE,    /* Half from start and half from end */
};

struct rspamd_log_module {
	char *mname;
	unsigned int id;
};

struct rspamd_log_modules {
	unsigned char *bitset;
	unsigned int bitset_len;       /* Number of BITS used in bitset */
	unsigned int bitset_allocated; /* Size of bitset allocated in BYTES */
	GHashTable *modules;
};

struct rspamd_logger_error_elt {
	int completed;
	GQuark ptype;
	pid_t pid;
	double ts;
	char id[RSPAMD_LOG_ID_LEN + 1];
	char module[9];
	char message[];
};

struct rspamd_logger_error_log {
	struct rspamd_logger_error_elt *elts;
	rspamd_mempool_t *pool;
	uint32_t max_elts;
	uint32_t elt_len;
	/* Avoid false cache sharing */
	unsigned char __padding[64 - sizeof(gpointer) * 2 - sizeof(uint64_t)];
	unsigned int cur_row;
};

/**
 * Static structure that store logging parameters
 * It is NOT shared between processes and is created by main process
 */
struct rspamd_logger_s {
	struct rspamd_logger_funcs ops;
	int log_level;

	struct rspamd_logger_error_log *errlog;
	struct rspamd_cryptobox_pubkey *pk;
	struct rspamd_cryptobox_keypair *keypair;

	unsigned int flags;
	gboolean closed;
	gboolean enabled;
	gboolean is_debug;
	gboolean no_lock;

	/* Log tag configuration */
	unsigned int max_log_tag_len;
	enum rspamd_log_tag_strip_policy log_tag_strip_policy;

	pid_t pid;
	const char *process_type;
	struct rspamd_radix_map_helper *debug_ip;
	rspamd_mempool_mutex_t *mtx;
	rspamd_mempool_t *pool;
	uint64_t log_cnt[4];
};

/*
 * Common logging prototypes
 */

/*
 * File logging
 */
void *rspamd_log_file_init(rspamd_logger_t *logger, struct rspamd_config *cfg,
						   uid_t uid, gid_t gid, GError **err);
void *rspamd_log_file_reload(rspamd_logger_t *logger, struct rspamd_config *cfg,
							 gpointer arg, uid_t uid, gid_t gid, GError **err);
void rspamd_log_file_dtor(rspamd_logger_t *logger, gpointer arg);
bool rspamd_log_file_log(const char *module, const char *id,
						 const char *function,
						 int level_flags,
						 const char *message,
						 gsize mlen,
						 rspamd_logger_t *rspamd_log,
						 gpointer arg);
bool rspamd_log_file_on_fork(rspamd_logger_t *logger, struct rspamd_config *cfg,
							 gpointer arg, GError **err);

struct rspamd_logger_iov_thrash_stack {
	struct rspamd_logger_iov_thrash_stack *prev;
	char data[0];
};
#define RSPAMD_LOGGER_MAX_IOV 8
struct rspamd_logger_iov_ctx {
	struct iovec iov[RSPAMD_LOGGER_MAX_IOV];
	int niov;
	struct rspamd_logger_iov_thrash_stack *thrash_stack;
};
/**
 * Fills IOV of logger (usable for file/console logging)
 * Warning: this function is NOT reentrant, do not call it twice from a single moment of execution
 * @param iov filled by this function
 * @param module
 * @param id
 * @param function
 * @param level_flags
 * @param message
 * @param mlen
 * @param rspamd_log
 * @return number of iov elements being filled
 */
void rspamd_log_fill_iov(struct rspamd_logger_iov_ctx *iov_ctx,
						 double ts,
						 const char *module, const char *id,
						 const char *function,
						 int level_flags,
						 const char *message,
						 gsize mlen,
						 rspamd_logger_t *rspamd_log);

/**
 * Frees IOV context
 * @param iov_ctx
 */
void rspamd_log_iov_free(struct rspamd_logger_iov_ctx *iov_ctx);
/**
 * Escape log line by replacing unprintable characters to hex escapes like \xNN
 * @param src
 * @param srclen
 * @param dst
 * @param dstlen
 * @return end of the escaped buffer
 */
char *rspamd_log_line_hex_escape(const unsigned char *src, gsize srclen,
								 char *dst, gsize dstlen);
/**
 * Returns number of characters to be escaped, e.g. a caller can allocate a new buffer
 * the desired number of characters
 * @param src
 * @param srclen
 * @return number of characters to be escaped
 */
gsize rspamd_log_line_need_escape(const unsigned char *src, gsize srclen);

static const struct rspamd_logger_funcs file_log_funcs = {
	.init = rspamd_log_file_init,
	.dtor = rspamd_log_file_dtor,
	.reload = rspamd_log_file_reload,
	.log = rspamd_log_file_log,
	.on_fork = rspamd_log_file_on_fork,
};

/*
 * Syslog logging
 */
void *rspamd_log_syslog_init(rspamd_logger_t *logger, struct rspamd_config *cfg,
							 uid_t uid, gid_t gid, GError **err);
void *rspamd_log_syslog_reload(rspamd_logger_t *logger, struct rspamd_config *cfg,
							   gpointer arg, uid_t uid, gid_t gid, GError **err);
void rspamd_log_syslog_dtor(rspamd_logger_t *logger, gpointer arg);
bool rspamd_log_syslog_log(const char *module, const char *id,
						   const char *function,
						   int level_flags,
						   const char *message,
						   gsize mlen,
						   rspamd_logger_t *rspamd_log,
						   gpointer arg);

static const struct rspamd_logger_funcs syslog_log_funcs = {
	.init = rspamd_log_syslog_init,
	.dtor = rspamd_log_syslog_dtor,
	.reload = rspamd_log_syslog_reload,
	.log = rspamd_log_syslog_log,
	.on_fork = NULL,
};

/*
 * Console logging
 */
void *rspamd_log_console_init(rspamd_logger_t *logger, struct rspamd_config *cfg,
							  uid_t uid, gid_t gid, GError **err);
void *rspamd_log_console_reload(rspamd_logger_t *logger, struct rspamd_config *cfg,
								gpointer arg, uid_t uid, gid_t gid, GError **err);
void rspamd_log_console_dtor(rspamd_logger_t *logger, gpointer arg);
bool rspamd_log_console_log(const char *module, const char *id,
							const char *function,
							int level_flags,
							const char *message,
							gsize mlen,
							rspamd_logger_t *rspamd_log,
							gpointer arg);

static const struct rspamd_logger_funcs console_log_funcs = {
	.init = rspamd_log_console_init,
	.dtor = rspamd_log_console_dtor,
	.reload = rspamd_log_console_reload,
	.log = rspamd_log_console_log,
	.on_fork = NULL,
};

#endif
