--[[
Copyright (c) 2016, Andrew Lewis <nerf@judo.za.org>
Copyright (c) 2016, Vsevolod Stakhov <vsevolod@highsecure.ru>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
]]--

if confighelp then
  return
end

-- A plugin that provides common header manipulations

local logger = require "rspamd_logger"
local util = require "rspamd_util"
local N = 'milter_headers'
local E = {}

local HOSTNAME = util.get_hostname()

local settings = {
  skip_local = true,
  skip_authenticated = true,
  local_headers = {},
  authenticated_headers = {},
  extended_headers_rcpt = {},
  routines = {
    ['remove-header'] = {
      remove = 1,
    },
    ['x-spamd-result'] = {
      header = 'X-Spamd-Result',
      remove = 1,
    },
    ['x-rspamd-server'] = {
      header = 'X-Rspamd-Server',
      remove = 1,
    },
    ['x-rspamd-queue-id'] = {
      header = 'X-Rspamd-Queue-Id',
      remove = 1,
    },
    ['spam-header'] = {
      header = 'Deliver-To',
      value = 'Junk',
      remove = 1,
    },
    ['x-virus'] = {
      header = 'X-Virus',
      remove = 1,
      symbols = {}, -- needs config
    },
    ['x-spamd-bar'] = {
      header = 'X-Spamd-Bar',
      positive = '+',
      negative = '-',
      neutral = '/',
      remove = 1,
    },
    ['x-spam-level'] = {
      header = 'X-Spam-Level',
      char = '*',
      remove = 1,
    },
    ['x-spam-status'] = {
      header = 'X-Spam-Status',
      remove = 1,
    },
    ['authentication-results'] = {
      header = 'Authentication-Results',
      remove = 1,
      spf_symbols = {
        pass = 'R_SPF_ALLOW',
        fail = 'R_SPF_FAIL',
        softfail = 'R_SPF_SOFTFAIL',
        neutral = 'R_SPF_NEUTRAL',
        temperror = 'R_SPF_DNSFAIL',
        none = 'R_SPF_NA',
        permerror = 'R_SPF_PERMFAIL',
      },
      dkim_symbols = {
        pass = 'R_DKIM_ALLOW',
        fail = 'R_DKIM_REJECT',
        temperror = 'R_DKIM_TEMPFAIL',
        none = 'R_DKIM_NA',
        permerror = 'R_DKIM_PERMFAIL',
      },
      dmarc_symbols = {
        pass = 'DMARC_POLICY_ALLOW',
        permerror = 'DMARC_BAD_POLICY',
        temperror = 'DMARC_DNSFAIL',
        none = 'DMARC_NA',
        reject = 'DMARC_POLICY_REJECT',
        softfail = 'DMARC_POLICY_SOFTFAIL',
        quarantine = 'DMARC_POLICY_QUARANTINE',
      },
    },
  },
}

local active_routines = {}
local custom_routines = {}

local function milter_headers(task)

  local function skip_wanted(hdr)

    local function match_extended_headers_rcpt()
      local rcpts = task:get_recipients('smtp')
      if not rcpts then return false end
      local found
      for _, r in ipairs(rcpts) do
        found = false
        for k, v in pairs(settings.extended_headers_rcpt) do
          for _, ehr in ipairs(v) do
            if r[k] == ehr then
              found = true
              break
            end
          end
          if found then break end
        end
        if not found then break end
      end
      return found
    end

    if settings.extended_headers_rcpt and match_extended_headers_rcpt() then
      return false
    end

    if settings.skip_local and not settings.local_headers[hdr] then
      local ip = task:get_ip()
      if (ip and ip:is_local()) then return true end
    end

    if settings.skip_authenticated and not settings.authenticated_headers[hdr] then
      if task:get_user() ~= nil then return true end
    end

    return false

  end

  local routines, common, add, remove = {}, {}, {}, {}

  routines['x-spamd-result'] = function()
    if skip_wanted('x-spamd-result') then return end
    if not common.symbols then
      common.symbols = task:get_symbols_all()
      common['metric_score'] = task:get_metric_score('default')
      common['metric_action'] = task:get_metric_action('default')
    end
    if settings.routines['x-spamd-result'].remove then
      remove[settings.routines['x-spamd-result'].header] = settings.routines['x-spamd-result'].remove
    end
    local buf = {}
    table.insert(buf, table.concat({
      'default: ', (common['metric_action'] == 'reject') and 'True' or 'False', ' [',
      string.format('%.2f', common['metric_score'][1]), ' / ', string.format('%.2f', common['metric_score'][2]), ']'
    }))
    for _, s in ipairs(common.symbols) do
      if not s.options then s.options = {} end
      table.insert(buf, table.concat({
        ' ', s.name, '(', string.format('%.2f', s.score), ')[', table.concat(s.options, ','), ']',
      }))
    end
    add[settings.routines['x-spamd-result'].header] = table.concat(buf, '\n')
  end

  routines['x-rspamd-queue-id'] = function()
    if skip_wanted('x-rspamd-queue-id') then return end
    if common.queue_id ~= false then
      common.queue_id = task:get_queue_id()
      if not common.queue_id then
        common.queue_id = false
      end
    end
    if settings.routines['x-rspamd-queue-id'].remove then
      remove[settings.routines['x-rspamd-queue-id'].header] = settings.routines['x-rspamd-queue-id'].remove
    end
    if common.queue_id then
      add[settings.routines['x-rspamd-queue-id'].header] = common.queue_id
    end
  end

  routines['remove-header'] = function()
    if skip_wanted('remove-header') then return end
    if settings.routines['remove-header'].header and settings.routines['remove-header'].remove then
      remove[settings.routines['remove-header'].header] = settings.routines['remove-header'].remove
    end
  end

  routines['x-rspamd-server'] = function()
    if skip_wanted('x-rspamd-server') then return end
    if settings.routines['x-rspamd-server'].remove then
      remove[settings.routines['x-rspamd-server'].header] = settings.routines['x-rspamd-server'].remove
    end
    add[settings.routines['x-rspamd-server'].header] = HOSTNAME
  end

  routines['x-spamd-bar'] = function()
    if skip_wanted('x-rspamd-bar') then return end
    if not common['metric_score'] then
      common['metric_score'] = task:get_metric_score('default')
    end
    local score = common['metric_score'][1]
    local spambar
    if score <= -1 then
      spambar = string.rep(settings.routines['x-spamd-bar'].negative, score*-1)
    elseif score >= 1 then
      spambar = string.rep(settings.routines['x-spamd-bar'].positive, score)
    else
      spambar = settings.routines['x-spamd-bar'].neutral
    end
    if settings.routines['x-spamd-bar'].remove then
      remove[settings.routines['x-spamd-bar'].header] = settings.routines['x-spamd-bar'].remove
    end
    if spambar ~= '' then
      add[settings.routines['x-spamd-bar'].header] = spambar
    end
  end

  routines['x-spam-level'] = function()
    if skip_wanted('x-spam-level') then return end
    if not common['metric_score'] then
      common['metric_score'] = task:get_metric_score('default')
    end
    local score = common['metric_score'][1]
    if score < 1 then
      return nil, {}, {}
    end
    if settings.routines['x-spam-level'].remove then
      remove[settings.routines['x-spam-level'].header] = settings.routines['x-spam-level'].remove
    end
    add[settings.routines['x-spam-level'].header] = string.rep(settings.routines['x-spam-level'].char, score)
  end

  local function spam_header (class, name, value, remove_v)
    if skip_wanted(class) then return end
    if not common['metric_action'] then
      common['metric_action'] = task:get_metric_action('default')
    end
    if remove_v then
      remove[name] = remove_v
    end
    local action = common['metric_action']
    if action ~= 'no action' and action ~= 'greylist' then
      add[name] = value
    end
  end

  routines['spam-header'] = function()
    spam_header('spam-header', settings.routines['spam-header'].header, settings.routines['spam-header'].value, settings.routines['spam-header'].remove)
  end

  routines['x-virus'] = function()
    if skip_wanted('x-virus') then return end
    if not common.symbols then
      common.symbols = {}
    end
    if settings.routines['x-virus'].remove then
      remove[settings.routines['x-virus'].header] = settings.routines['x-virus'].remove
    end
    local virii = {}
    for _, sym in ipairs(settings.routines['x-virus'].symbols) do
      if not (common.symbols[sym] == false) then
        local s = task:get_symbol(sym)
        if not s then
          common.symbols[sym] = false
        else
          common.symbols[sym] = s
          if (((s or E)[1] or E).options or E)[1] then
            table.insert(virii, s[1].options[1])
          else
            table.insert(virii, 'unknown')
          end
        end
      end
    end
    if #virii > 0 then
      add[settings.routines['x-virus'].header] = table.concat(virii, ',')
    end
  end

  routines['x-spam-status'] = function()
    if skip_wanted('x-spam-status') then return end
    if not common['metric_score'] then
      common['metric_score'] = task:get_metric_score('default')
    end
    if not common['metric_action'] then
      common['metric_action'] = task:get_metric_action('default')
    end
    local score = common['metric_score'][1]
    local action = common['metric_action']
    local is_spam
    local spamstatus
    if action ~= 'no action' and action ~= 'greylist' then
      is_spam = 'Yes'
    else
      is_spam = 'No'
    end
    spamstatus = is_spam .. ', score=' .. string.format('%.2f', score)
    if settings.routines['x-spam-status'].remove then
      remove[settings.routines['x-spam-status'].header] = settings.routines['x-spam-status'].remove
    end
    add[settings.routines['x-spam-status'].header] = spamstatus
  end

  routines['authentication-results'] = function()
    if skip_wanted('authentication-results') then return end
    local ar = require "auth_results"

    if settings.routines['authentication-results'].remove then
      remove[settings.routines['authentication-results'].header] =
          settings.routines['authentication-results'].remove
    end

    local res = ar.gen_auth_results(task,
      settings.routines['authentication-results'])

    if res then
      add[settings.routines['authentication-results'].header] = res
    end
  end

  for _, n in ipairs(active_routines) do
    local ok, err
    if custom_routines[n] then
      local to_add, to_remove, common_in
      ok, err, to_add, to_remove, common_in = pcall(custom_routines[n], task, common)
      if ok then
        for k, v in pairs(to_add) do
          add[k] = v
        end
        for k, v in pairs(to_remove) do
          add[k] = v
        end
        for k, v in pairs(common_in) do
          if type(v) == 'table' then
            if not common[k] then
              common[k] = {}
            end
            for kk, vv in pairs(v) do
              common[k][kk] = vv
            end
          else
            common[k] = v
          end
        end
      end
    else
      ok, err = pcall(routines[n])
    end
    if not ok then
      logger.errx(task, 'call to %s failed: %s', n, err)
    end
  end

  if not next(add) then add = nil end
  if not next(remove) then remove = nil end
  if add or remove then
    task:set_milter_reply({
      add_headers = add,
      remove_headers = remove
    })
  end
end

local opts = rspamd_config:get_all_opt(N) or rspamd_config:get_all_opt('rmilter_headers')
if not opts then return end

local have_routine = {}
local function activate_routine(s)
  if settings.routines[s] or custom_routines[s] then
    if not have_routine[s] then
      have_routine[s] = true
      table.insert(active_routines, s)
      if (opts.routines and opts.routines[s]) then
        for k, v in pairs(opts.routines[s]) do
          settings.routines[s][k] = v
        end
      end
    end
  else
    logger.errx(rspamd_config, 'routine "%s" does not exist', s)
  end
end
if opts['extended_spam_headers'] then
  activate_routine('x-spamd-result')
  activate_routine('x-rspamd-server')
  activate_routine('x-rspamd-queue-id')
end
if type(opts['use']) == 'string' then
  opts['use'] = {opts['use']}
elseif (type(opts['use']) == 'table' and not opts['use'][1] and not next(active_routines)) then
  logger.debugm(N, rspamd_config, 'no functions are enabled')
  return
end
if type(opts['use']) ~= 'table' then
  logger.errx(rspamd_config, 'unexpected type for "use" option: %s', type(opts['use']))
  return
end
if type(opts['local_headers']) == 'table' and opts['local_headers'][1] then
  for _, h in ipairs(opts['local_headers']) do
    settings.local_headers[h] = true
  end
end
if type(opts['authenticated_headers']) == 'table' and opts['authenticated_headers'][1] then
  for _, h in ipairs(opts['authenticated_headers']) do
    settings.authenticated_headers[h] = true
  end
end
if type(opts['custom']) == 'table' then
  for k, v in pairs(opts['custom']) do
    local f, err = load(v)
    if not f then
      logger.errx(rspamd_config, 'could not load "%s": %s', k, err)
    else
      custom_routines[k] = f()
    end
  end
end
if opts['extended_spam_headers'] then
  activate_routine('x-spamd-result')
  activate_routine('x-rspamd-server')
  activate_routine('x-rspamd-queue-id')
end
if opts['skip_local'] then
  settings.skip_local = true
end
if opts['skip_authenticated'] then
  settings.skip_authenticated = true
end
for _, s in ipairs(opts['use']) do
  if not have_routine[s] then
    activate_routine(s)
  end
end
if (#active_routines < 1) then
  logger.errx(rspamd_config, 'no active routines')
  return
end
logger.infox(rspamd_config, 'active routines [%s]', table.concat(active_routines, ','))
if type(opts['extended_headers_rcpt']) == 'string' then
  opts['extended_headers_rcpt'] = {opts['extended_headers_rcpt']}
end
if type(opts['extended_headers_rcpt']) == 'table' and opts['extended_headers_rcpt'][1] then
  for _, e in ipairs(opts['extended_headers_rcpt']) do
    if string.find(e, '^[^@]+@[^@]+$') then
      if not settings.extended_headers_rcpt.addr then
        settings.extended_headers_rcpt.addr = {}
      end
      table.insert(settings.extended_headers_rcpt['addr'], e)
    elseif string.find(e, '^[^@]+$') then
      if not settings.extended_headers_rcpt.user then
        settings.extended_headers_rcpt.user = {}
      end
      table.insert(settings.extended_headers_rcpt['user'], e)
    else
      local d = string.match(e, '^@([^@]+)$')
      if d then
        if not settings.extended_headers_rcpt.domain then
          settings.extended_headers_rcpt.domain = {}
        end
        table.insert(settings.extended_headers_rcpt['domain'], d)
      else
        logger.errx(rspamd_config, 'extended_headers_rcpt: unexpected entry: %s', e)
      end
    end
  end
end
rspamd_config:register_symbol({
  name = 'MILTER_HEADERS',
  type = 'postfilter',
  callback = milter_headers,
  priority = 10
})
