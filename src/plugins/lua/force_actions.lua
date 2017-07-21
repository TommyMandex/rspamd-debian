--[[
Copyright (c) 2017, Andrew Lewis <nerf@judo.za.org>
Copyright (c) 2017, Vsevolod Stakhov <vsevolod@highsecure.ru>

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

-- A plugin that forces actions

if confighelp then
  return
end

local E = {}
local N = 'force_actions'

local fun = require "fun"
local rspamd_cryptobox_hash = require "rspamd_cryptobox_hash"
local rspamd_expression = require "rspamd_expression"
local rspamd_logger = require "rspamd_logger"

local function gen_cb(expr, act, pool, message, subject, raction, honor)

  local function parse_atom(str)
    local atom = table.concat(fun.totable(fun.take_while(function(c)
      if string.find(', \t()><+!|&\n', c) then
        return false
      end
      return true
    end, fun.iter(str))), '')
    return atom
  end

  local function process_atom(atom, task)
    local f_ret = task:has_symbol(atom)
    if f_ret then
      return 1
    end
    return 0
  end

  local e, err = rspamd_expression.create(expr, {parse_atom, process_atom}, pool)
  if err then
    rspamd_logger.errx(rspamd_config, 'Couldnt create expression [%1]: %2', expr, err)
    return
  end

  return function(task)

    local cact = task:get_metric_action('default')
    if cact == act then
      return false
    end
    if honor and honor[cact] then
      return false
    elseif raction and not raction[cact] then
      return false
    end

    if e:process(task) == 1 then
      if subject then
        task:set_metric_subject(subject)
      end
      if type(message) == 'string' then
        task:set_pre_result(act, message)
      else
        task:set_pre_result(act)
      end
      return true, act
    end

  end, e:atoms()

end

local function list_to_hash(list)
  if type(list) == 'table' then
    if list[1] then
      local h = {}
      for _, e in ipairs(list) do
        h[e] = true
      end
      return h
    else
      return list
    end
  elseif type(list) == 'string' then
    local h = {}
    h[list] = true
    return h
  end
end

local function configure_module()
  local opts = rspamd_config:get_all_opt(N)
  if not opts then
    return false
  end
  if type(opts.actions) == 'table' then
    rspamd_logger.warnx(rspamd_config, 'Processing legacy config')
    for action, expressions in pairs(opts.actions) do
      if type(expressions) == 'table' then
        for _, expr in ipairs(expressions) do
          local message, subject
          if type(expr) == 'table' then
            subject = expr[3]
            message = expr[2]
            expr = expr[1]
          else
            message = (opts.messages or E)[expr]
          end
          if type(expr) == 'string' then
            local cb, atoms = gen_cb(expr, action, rspamd_config:get_mempool(), message, subject)
            if cb and atoms then
              local h = rspamd_cryptobox_hash.create()
              h:update(expr)
              local name = 'FORCE_ACTION_' .. string.upper(string.sub(h:hex(), 1, 12))
              local id = rspamd_config:register_symbol({
                type = 'normal',
                name = name,
                callback = cb,
              })
              for _, a in ipairs(atoms) do
                rspamd_config:register_dependency(id, a)
              end
              rspamd_logger.infox(rspamd_config, 'Registered symbol %1 <%2> with dependencies [%3]', name, expr, table.concat(atoms, ','))
            end
          end
        end
      end
    end
  elseif type(opts.rules) == 'table' then
    for name, sett in pairs(opts.rules) do
      local action = sett.action
      local expr = sett.expression
      if action and expr then
        local subject = sett.subject
        local message = sett.message
        local raction = list_to_hash(sett.require_action)
        local honor = list_to_hash(sett.honor_action)
        local cb, atoms = gen_cb(expr, action, rspamd_config:get_mempool(), message, subject, raction, honor)
        if cb and atoms then
          local t = {}
          if (raction or honor) then
            t.type = 'postfilter'
            t.priority = 10
          else
            t.type = 'normal'
          end
          t.name = 'FORCE_ACTION_' .. name
          t.callback = cb
          local id = rspamd_config:register_symbol(t)
          if t.type == 'normal' then
            for _, a in ipairs(atoms) do
              rspamd_config:register_dependency(id, a)
            end
            rspamd_logger.infox(rspamd_config, 'Registered symbol %1 <%2> with dependencies [%3]', name, expr, table.concat(atoms, ','))
          else
            rspamd_logger.infox(rspamd_config, 'Registered symbol %1 <%2> as postfilter', name, expr)
          end
        end
      end
    end
  end
end

configure_module()
