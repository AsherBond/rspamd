--[[
Copyright (c) 2024, Vsevolod Stakhov <vsevolod@rspamd.com>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
]] --

local N = "gpt"
local E = {}

if confighelp then
  rspamd_config:add_example(nil, 'gpt',
      "Performs postfiltering using GPT model",
      [[
gpt {
  # Supported types: openai, ollama
  type = "openai";
  # Your key to access the API
  api_key = "xxx";
  # Model name
  model = "gpt-4o-mini";
  # Maximum tokens to generate
  max_tokens = 1000;
  # Temperature for sampling
  temperature = 0.0;
  # Timeout for requests
  timeout = 10s;
  # Prompt for the model (use default if not set)
  prompt = "xxx";
  # Custom condition (lua function)
  condition = "xxx";
  # Autolearn if gpt classified
  autolearn = true;
  # Reply conversion (lua code)
  reply_conversion = "xxx";
  # URL for the API
  url = "https://api.openai.com/v1/chat/completions";
  # Check messages with passthrough result
  allow_passthrough = false;
  # Check messages that are apparent ham (no action and negative score)
  allow_ham = false;
  # default send response_format field { type = "json_object" }
  include_response_format = true,
  # Add header with reason (null to disable)
  reason_header = "X-GPT-Reason";
}
  ]])
  return
end

local lua_util = require "lua_util"
local rspamd_http = require "rspamd_http"
local rspamd_logger = require "rspamd_logger"
local lua_mime = require "lua_mime"
local ucl = require "ucl"
local fun = require "fun"

-- Exclude checks if one of those is found
local default_symbols_to_except = {
  BAYES_SPAM = 0.9, -- We already know that it is a spam, so we can safely skip it, but no same logic for HAM!
  WHITELIST_SPF = -1,
  WHITELIST_DKIM = -1,
  WHITELIST_DMARC = -1,
  FUZZY_DENIED = -1,
  REPLY = -1,
  BOUNCE = -1,
}

local settings = {
  type = 'openai',
  api_key = nil,
  model = 'gpt-4o-mini',
  max_tokens = 1000,
  temperature = 0.0,
  timeout = 10,
  prompt = nil,
  condition = nil,
  autolearn = false,
  reason_header = nil,
  url = 'https://api.openai.com/v1/chat/completions',
  symbols_to_except = nil,
  allow_passthrough = false,
  allow_ham = false,
}

local function default_condition(task)
  -- Check result
  -- 1) Skip passthrough
  -- 2) Skip already decided as spam
  -- 3) Skip already decided as ham
  local result = task:get_metric_result()
  if result then
    if result.passthrough and not settings.allow_passthrough then
      return false, 'passthrough'
    end
    local score = result.score
    local action = result.action

    if action == 'reject' and result.npositive > 1 then
      return false, 'already decided as spam'
    end

    if (action == 'no action' and score < 0) and not settings.allow_ham then
      return false, 'negative score, already decided as ham'
    end
  end
  -- We also exclude some symbols
  for s, required_weight in pairs(settings.symbols_to_except) do
    if task:has_symbol(s) then
      if required_weight > 0 then
        -- Also check score
        local sym = task:get_symbol(s) or E
        -- Must exist as we checked it before with `has_symbol`
        if sym.weight then
          if math.abs(sym.weight) >= required_weight then
            return false, 'skip as "' .. s .. '" is found (weight: ' .. sym.weight .. ')'
          end
        end
        lua_util.debugm(N, task, 'symbol %s has weight %s, but required %s', s,
            sym.weight, required_weight)
      else
        return false, 'skip as "' .. s .. '" is found'
      end
    end
  end

  -- Check if we have text at all
  local sel_part = lua_mime.get_displayed_text_part(task)

  if not sel_part then
    return false, 'no text part found'
  end

  -- Check limits and size sanity
  local nwords = sel_part:get_words_count()

  if nwords < 5 then
    return false, 'less than 5 words'
  end

  if nwords > settings.max_tokens then
    -- We need to truncate words (sometimes get_words_count returns a different number comparing to `get_words`)
    local words = sel_part:get_words('norm')
    nwords = #words
    if nwords > settings.max_tokens then
      return true, table.concat(words, ' ', 1, settings.max_tokens)
    end
  end
  return true, sel_part:get_content_oneline()
end

local function maybe_extract_json(str)
  -- Find the first opening brace
  local startPos, endPos = str:find('json%s*{')
  if not startPos then
    startPos, endPos = str:find('{')
  end
  if not startPos then
    return nil
  end

  startPos = endPos - 1
  local openBraces = 0
  endPos = startPos
  local len = #str

  -- Iterate through the string to find matching braces
  for i = startPos, len do
    local char = str:sub(i, i)
    if char == "{" then
      openBraces = openBraces + 1
    elseif char == "}" then
      openBraces = openBraces - 1
      -- When we find the matching closing brace
      if openBraces == 0 then
        endPos = i
        break
      end
    end
  end

  -- If we found a complete JSON-like structure
  if openBraces == 0 then
    return str:sub(startPos, endPos)
  end

  return nil
end

local function default_conversion(task, input)
  local parser = ucl.parser()
  local res, err = parser:parse_string(input)
  if not res then
    rspamd_logger.errx(task, 'cannot parse reply: %s', err)
    return
  end
  local reply = parser:get_object()
  if not reply then
    rspamd_logger.errx(task, 'cannot get object from reply')
    return
  end

  if type(reply.choices) ~= 'table' or type(reply.choices[1]) ~= 'table' then
    rspamd_logger.errx(task, 'no choices in reply')
    return
  end

  local first_message = reply.choices[1].message.content

  if not first_message then
    rspamd_logger.errx(task, 'no content in the first message')
    return
  end

  -- Apply heuristic to extract JSON
  first_message = maybe_extract_json(first_message) or first_message

  parser = ucl.parser()
  res, err = parser:parse_string(first_message)
  if not res then
    rspamd_logger.errx(task, 'cannot parse JSON gpt reply: %s', err)
    return
  end

  reply = parser:get_object()

  if type(reply) == 'table' and reply.probability then
    lua_util.debugm(N, task, 'extracted probability: %s', reply.probability)
    local spam_score = tonumber(reply.probability)

    if not spam_score then
      -- Maybe we need GPT to convert GPT reply here?
      if reply.probability == "high" then
        spam_score = 0.9
      elseif reply.probability == "low" then
        spam_score = 0.1
      else
        rspamd_logger.infox("cannot convert to spam probability: %s", reply.probability)
      end
    end

    if type(reply.usage) == 'table' then
      rspamd_logger.infox(task, 'usage: %s tokens', reply.usage.total_tokens)
    end

    return spam_score, reply.reason
  end

  rspamd_logger.errx(task, 'cannot convert spam score: %s', first_message)
  return
end

local function ollama_conversion(task, input)
  local parser = ucl.parser()
  local res, err = parser:parse_string(input)
  if not res then
    rspamd_logger.errx(task, 'cannot parse reply: %s', err)
    return
  end
  local reply = parser:get_object()
  if not reply then
    rspamd_logger.errx(task, 'cannot get object from reply')
    return
  end

  if type(reply.message) ~= 'table' then
    rspamd_logger.errx(task, 'bad message in reply')
    return
  end

  local first_message = reply.message.content

  if not first_message then
    rspamd_logger.errx(task, 'no content in the first message')
    return
  end

  -- Apply heuristic to extract JSON
  first_message = maybe_extract_json(first_message) or first_message

  parser = ucl.parser()
  res, err = parser:parse_string(first_message)
  if not res then
    rspamd_logger.errx(task, 'cannot parse JSON gpt reply: %s', err)
    return
  end

  reply = parser:get_object()

  if type(reply) == 'table' and reply.probability then
    lua_util.debugm(N, task, 'extracted probability: %s', reply.probability)
    local spam_score = tonumber(reply.probability)

    if not spam_score then
      -- Maybe we need GPT to convert GPT reply here?
      if reply.probability == "high" then
        spam_score = 0.9
      elseif reply.probability == "low" then
        spam_score = 0.1
      else
        rspamd_logger.infox("cannot convert to spam probability: %s", reply.probability)
      end
    end

    if type(reply.usage) == 'table' then
      rspamd_logger.infox(task, 'usage: %s tokens', reply.usage.total_tokens)
    end

    return spam_score, reply.reason
  end

  rspamd_logger.errx(task, 'cannot convert spam score: %s', first_message)
  return
end

local function check_consensus(task, results)
  for _, result in ipairs(results) do
    if not result.checked then
      return
    end
  end

  local nspam, nham = 0, 0
  local max_spam_prob, max_ham_prob = 0, 0
  local reasons = {}

  for _, result in ipairs(results) do
    if result.success then
      if result.probability > 0.5 then
        nspam = nspam + 1
        max_spam_prob = math.max(max_spam_prob, result.probability)
        lua_util.debugm(N, task, "model: %s; spam: %s; reason: '%s'",
            result.model, result.probability, result.reason)
      else
        nham = nham + 1
        max_ham_prob = math.min(max_ham_prob, result.probability)
        lua_util.debugm(N, task, "model: %s; ham: %s; reason: '%s'",
            result.model, result.probability, result.reason)
      end

      if result.reason then
        table.insert(reasons, result.reason)
      end
    end
  end

  lua_util.shuffle(reasons)
  local reason = reasons[1] or nil

  if nspam > nham and max_spam_prob > 0.75 then
    task:insert_result('GPT_SPAM', (max_spam_prob - 0.75) * 4, tostring(max_spam_prob))
    if settings.autolearn then
      task:set_flag("learn_spam")
    end

    if reason and settings.reason_header then
      lua_mime.modify_headers(task,
          { add = { [settings.reason_header] = { value = 'value', order = 1 } } })
    end
  elseif nham > nspam and max_ham_prob < 0.25 then
    task:insert_result('GPT_HAM', (0.25 - max_ham_prob) * 4, tostring(max_ham_prob))
    if settings.autolearn then
      task:set_flag("learn_ham")
    end
    if reason and settings.reason_header then
      lua_mime.modify_headers(task,
          { add = { [settings.reason_header] = { value = 'value', order = 1 } } })
    end
  else
    -- No consensus
    lua_util.debugm(N, task, "no consensus")
  end

end

local function get_meta_llm_content(task)
  local url_content = "Url domains: no urls found"
  if task:has_urls() then
    local urls = lua_util.extract_specific_urls { task = task, limit = 5, esld_limit = 1 }
    url_content = "Url domains: " .. table.concat(fun.totable(fun.map(function(u)
      return u:get_tld() or ''
    end, urls or {})), ', ')
  end

  local from_or_empty = ((task:get_from('mime') or E)[1] or E)
  local from_content = string.format('From: %s <%s>', from_or_empty.name, from_or_empty.addr)
  lua_util.debugm(N, task, "gpt urls: %s", url_content)
  lua_util.debugm(N, task, "gpt from: %s", from_content)

  return url_content, from_content
end

local function default_llm_check(task)
  local ret, content = settings.condition(task)

  if not ret then
    rspamd_logger.info(task, "skip checking gpt as the condition is not met: %s", content)
    return
  end

  if not content then
    lua_util.debugm(N, task, "no content to send to gpt classification")
    return
  end

  lua_util.debugm(N, task, "sending content to gpt: %s", content)

  local upstream

  local results = {}

  local function gen_reply_closure(model, idx)
    return function(err, code, body)
      results[idx].checked = true
      if err then
        rspamd_logger.errx(task, '%s: request failed: %s', model, err)
        upstream:fail()
        check_consensus(task, results)
        return
      end

      upstream:ok()
      lua_util.debugm(N, task, "%s: got reply: %s", model, body)
      if code ~= 200 then
        rspamd_logger.errx(task, 'bad reply: %s', body)
        return
      end

      local reply, reason = settings.reply_conversion(task, body)

      results[idx].model = model

      if reply then
        results[idx].success = true
        results[idx].probability = reply
        results[idx].reason = reason
      end

      check_consensus(task, results)
    end
  end

  local from_content, url_content = get_meta_llm_content(task)

  local body = {
    model = settings.model,
    max_tokens = settings.max_tokens,
    temperature = settings.temperature,
    messages = {
      {
        role = 'system',
        content = settings.prompt
      },
      {
        role = 'user',
        content = 'Subject: ' .. task:get_subject() or '',
      },
      {
        role = 'user',
        content = from_content,
      },
      {
        role = 'user',
        content = url_content,
      },
      {
        role = 'user',
        content = content
      }
    }
  }

  -- Conditionally add response_format
  if settings.include_response_format then
    body.response_format = { type = "json_object" }
  end

  if type(settings.model) == 'string' then
    settings.model = { settings.model }
  end

  upstream = settings.upstreams:get_upstream_round_robin()
  for idx, model in ipairs(settings.model) do
    results[idx] = {
      success = false,
      checked = false
    }
    body.model = model
    local http_params = {
      url = settings.url,
      mime_type = 'application/json',
      timeout = settings.timeout,
      log_obj = task,
      callback = gen_reply_closure(model, idx),
      headers = {
        ['Authorization'] = 'Bearer ' .. settings.api_key,
      },
      keepalive = true,
      body = ucl.to_format(body, 'json-compact', true),
      task = task,
      upstream = upstream,
      use_gzip = true,
    }

    if not rspamd_http.request(http_params) then
      results[idx].checked = true
    end

  end
end

local function ollama_check(task)
  local ret, content = settings.condition(task)

  if not ret then
    rspamd_logger.info(task, "skip checking gpt as the condition is not met: %s", content)
    return
  end

  if not content then
    lua_util.debugm(N, task, "no content to send to gpt classification")
    return
  end

  lua_util.debugm(N, task, "sending content to gpt: %s", content)

  local upstream
  local results = {}

  local function gen_reply_closure(model, idx)
    return function(err, code, body)
      results[idx].checked = true
      if err then
        rspamd_logger.errx(task, '%s: request failed: %s', model, err)
        upstream:fail()
        check_consensus(task, results)
        return
      end

      upstream:ok()
      lua_util.debugm(N, task, "%s: got reply: %s", model, body)
      if code ~= 200 then
        rspamd_logger.errx(task, 'bad reply: %s', body)
        return
      end

      local reply, reason = settings.reply_conversion(task, body)

      results[idx].model = model

      if reply then
        results[idx].success = true
        results[idx].probability = reply
        results[idx].reason = reason
      end

      check_consensus(task, results)
    end
  end

  local from_content, url_content = get_meta_llm_content(task)

  if type(settings.model) == 'string' then
    settings.model = { settings.model }
  end

  local body = {
    stream = false,
    model = settings.model,
    max_tokens = settings.max_tokens,
    temperature = settings.temperature,
    messages = {
      {
        role = 'system',
        content = settings.prompt
      },
      {
        role = 'user',
        content = 'Subject: ' .. task:get_subject() or '',
      },
      {
        role = 'user',
        content = from_content,
      },
      {
        role = 'user',
        content = url_content,
      },
      {
        role = 'user',
        content = content
      }
    }
  }

  for i, model in ipairs(settings.model) do
    -- Conditionally add response_format
    if settings.include_response_format then
      body.response_format = { type = "json_object" }
    end

    body.model = model

    upstream = settings.upstreams:get_upstream_round_robin()
    local http_params = {
      url = settings.url,
      mime_type = 'application/json',
      timeout = settings.timeout,
      log_obj = task,
      callback = gen_reply_closure(model, i),
      keepalive = true,
      body = ucl.to_format(body, 'json-compact', true),
      task = task,
      upstream = upstream,
      use_gzip = true,
    }

    rspamd_http.request(http_params)
  end
end

local function gpt_check(task)
  return settings.specific_check(task)
end

local types_map = {
  openai = {
    check = default_llm_check,
    condition = default_condition,
    conversion = default_conversion,
    require_passkey = true,
  },
  ollama = {
    check = ollama_check,
    condition = default_condition,
    conversion = ollama_conversion,
    require_passkey = false,
  },
}

local opts = rspamd_config:get_all_opt('gpt')
if opts then
  settings = lua_util.override_defaults(settings, opts)

  if not settings.prompt then
    settings.prompt = "You will be provided with the email message, subject, from and url domains, " ..
        "and your task is to evaluate the probability to be spam as number from 0 to 1, " ..
        "output result as JSON with 'probability' field and " ..
        "add 'reason' field with 1 sentence description why you have made that decision."
  end

  if not settings.symbols_to_except then
    settings.symbols_to_except = default_symbols_to_except
  end

  local llm_type = types_map[settings.type]
  if not llm_type then
    rspamd_logger.warnx(rspamd_config, 'unsupported gpt type: %s', settings.type)
    lua_util.disable_module(N, "config")
    return
  end
  settings.specific_check = llm_type.check

  if settings.condition then
    settings.condition = load(settings.condition)()
  else
    settings.condition = llm_type.condition
  end

  if settings.reply_conversion then
    settings.reply_conversion = load(settings.reply_conversion)()
  else
    settings.reply_conversion = llm_type.conversion
  end

  if not settings.api_key and llm_type.require_passkey then
    rspamd_logger.warnx(rspamd_config, 'no api_key is specified for LLM type %s, disabling module', settings.type)
    lua_util.disable_module(N, "config")

    return
  end

  settings.upstreams = lua_util.http_upstreams_by_url(rspamd_config:get_mempool(), settings.url)

  local id = rspamd_config:register_symbol({
    name = 'GPT_CHECK',
    type = 'postfilter',
    callback = gpt_check,
    priority = lua_util.symbols_priorities.medium,
    augmentations = { string.format("timeout=%f", settings.timeout or 0.0) },
  })

  rspamd_config:register_symbol({
    name = 'GPT_SPAM',
    type = 'virtual',
    parent = id,
    score = 5.0,
  })
  rspamd_config:register_symbol({
    name = 'GPT_HAM',
    type = 'virtual',
    parent = id,
    score = -2.0,
  })
end
