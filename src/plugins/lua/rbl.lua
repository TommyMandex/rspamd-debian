--[[
Copyright (c) 2011-2015, Vsevolod Stakhov <vsevolod@highsecure.ru>
Copyright (c) 2013-2015, Andrew Lewis <nerf@judo.za.org>

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

-- This plugin implements various types of RBL checks
-- Documentation can be found here:
-- https://rspamd.com/doc/modules/rbl.html

local E = {}
local N = 'rbl'

local rbls = {}
local local_exclusions = nil

local hash = require 'rspamd_cryptobox_hash'
local rspamd_logger = require 'rspamd_logger'
local rspamd_util = require 'rspamd_util'
local fun = require 'fun'
local default_monitored = '1.0.0.127'

local symbols = {
  dkim_allow_symbol = 'R_DKIM_ALLOW',
}

local dkim_config = rspamd_config:get_all_opt("dkim")
if (dkim_config or E).symbol_allow then
  symbols['dkim_allow_symbol'] = dkim_config['symbol_allow']
end

local function validate_dns(lstr)
  if lstr:match('%.%.') then
    return false
  end
  for v in lstr:gmatch('[^%.]+') do
    if not v:match('^[%w-]+$') or v:len() > 63
      or v:match('^-') or v:match('-$') then
      return false
    end
  end
  return true
end

local hash_alg = {
  sha1 = true,
  md5 = true,
  sha256 = true,
  sha384 = true,
  sha512 = true,
}

local function make_hash(data, specific)
  local h
  if not hash_alg[specific] then
    h = hash.create(data)
  else
    h = hash.create_specific(specific, data)
  end
  return h:hex()
end

local function is_excluded_ip(rip)
  if local_exclusions and local_exclusions:get_key(rip) then
    return true
  end
  return false
end

local function ip_to_rbl(ip, rbl)
  return table.concat(ip:inversed_str_octets(), '.') .. '.' .. rbl
end

local function rbl_cb (task)
  local function gen_rbl_callback(rule)
    return function (_, to_resolve, results, err)
      if err and (err ~= 'requested record is not found' and err ~= 'no records with this name') then
        rspamd_logger.errx(task, 'error looking up %s: %s', to_resolve, err)
      end
      if not results then
        rspamd_logger.debugm(N, task, 'DNS RESPONSE: label=%1 results=%2 error=%3 rbl=%4', to_resolve, false, err, rule['rbls'][1]['symbol'])
        return
      else
        rspamd_logger.debugm(N, task, 'DNS RESPONSE: label=%1 results=%2 error=%3 rbl=%4', to_resolve, true, err, rule['rbls'][1]['symbol'])
      end

      for _,rbl in ipairs(rule.rbls) do
        if rbl['returncodes'] == nil and rbl['symbol'] ~= nil then
          task:insert_result(rbl['symbol'], 1, to_resolve)
          return
        end
        for _,result in pairs(results) do
          local ipstr = result:to_string()
          local foundrc
          rspamd_logger.debugm(N, task, '%s DNS result %s', to_resolve, ipstr)
          for s,i in pairs(rbl['returncodes']) do
            if type(i) == 'string' then
              if string.find(ipstr, '^' .. i .. '$') then
                foundrc = i
                task:insert_result(s, 1, to_resolve .. ' : ' .. ipstr)
                break
              end
            elseif type(i) == 'table' then
              for _,v in pairs(i) do
                if string.find(ipstr, '^' .. v .. '$') then
                  foundrc = v
                  task:insert_result(s, 1, to_resolve .. ' : ' .. ipstr)
                  break
                end
              end
            end
          end
          if not foundrc then
            if rbl['unknown'] and rbl['symbol'] then
              task:insert_result(rbl['symbol'], 1, to_resolve)
            else
              rspamd_logger.errx(task, 'RBL %1 returned unknown result: %2',
                rbl['rbl'], ipstr)
            end
          end
        end
      end

      task:inc_dns_req()
    end
  end

  local params = {} -- indexed by rbl name

  local function gen_rbl_rule(to_resolve, rbl)
    rspamd_logger.debugm(N, task, 'DNS REQUEST: label=%1 rbl=%2', to_resolve, rbl['symbol'])
    if not params[to_resolve] then
      local nrule = {
        to_resolve = to_resolve,
        rbls = {rbl},
        forced = true,
      }
      nrule.callback = gen_rbl_callback(nrule)
      params[to_resolve] = nrule
    else
      table.insert(params[to_resolve].rbls, rbl)
    end

    return params[to_resolve]
  end

  local havegot = {}
  local notgot = {}

  local alive_rbls = fun.filter(function(_, rbl)
    if rbl.monitored then
      if not rbl.monitored:alive() then
        return false
      end
    end

    return true
  end, rbls)

  -- Now exclude rbls, that are disabled by configuration
  local enabled_rbls = fun.filter(function(_, rbl)
    if rbl['exclude_users'] then
      if not havegot['user'] and not notgot['user'] then
        havegot['user'] = task:get_user()
        if havegot['user'] == nil then
          notgot['user'] = true
        end
      end
      if havegot['user'] ~= nil then
        return false
      end
    end

    if (rbl['exclude_local'] or rbl['exclude_private_ips']) and not notgot['from'] then
      if not havegot['from'] then
        havegot['from'] = task:get_from_ip()
        if not havegot['from']:is_valid() then
          notgot['from'] = true
        end
      end
      if havegot['from'] and not notgot['from'] and ((rbl['exclude_local'] and
        is_excluded_ip(havegot['from'])) or (rbl['exclude_private_ips'] and
        havegot['from']:is_local())) then
        return false
      end
    end

    -- Helo checks
    if rbl['helo'] then
      if notgot['helo'] then
        return false
      end
      if not havegot['helo'] then
        if rbl['hash'] then
          havegot['helo'] = task:get_helo()
          if havegot['helo'] then
            havegot['helo'] = make_hash(havegot['helo'], rbl['hash'])
          else
            notgot['helo'] = true
            return false
          end
        else
          havegot['helo'] = task:get_helo()
          if havegot['helo'] == nil or not validate_dns(havegot['helo']) then
            havegot['helo'] = nil
            notgot['helo'] = true
            return false
          end
        end
      end
    elseif rbl['dkim'] then
      -- DKIM checks
      if notgot['dkim'] then
        return false
      end
      if not havegot['dkim'] then
        local das = task:get_symbol(symbols['dkim_allow_symbol'])
        if ((das or E)[1] or E).options then
          havegot['dkim'] = das[1]['options']
        else
          notgot['dkim'] = true
          return false
        end
      end
    elseif rbl['emails'] then
      -- Emails checks
      if notgot['emails'] then
        return false
      end
      if not havegot['emails'] then
        havegot['emails'] = task:get_emails()
        if havegot['emails'] == nil then
          notgot['emails'] = true
          return false
        end
      end
    elseif rbl['from'] then
      if notgot['from'] then
        return false
      end
      if not havegot['from'] then
        havegot['from'] = task:get_from_ip()
        if not havegot['from']:is_valid() then
          notgot['from'] = true
          return false
        end
      end
    elseif rbl['received'] then
      if notgot['received'] then
        return false
      end
      if not havegot['received'] then
        havegot['received'] = task:get_received_headers()
        if next(havegot['received']) == nil then
          notgot['received'] = true
          return false
        end
      end
    elseif rbl['rdns'] then
      if notgot['rdns'] then
        return false
      end
      if not havegot['rdns'] then
        havegot['rdns'] = task:get_hostname()
        if havegot['rdns'] == nil or havegot['rdns'] == 'unknown' then
          notgot['rdns'] = true
          return false
        end
      end
    end

    return true
  end, alive_rbls)

  -- Now we iterate over enabled rbls and fill params
  -- Helo RBLs
  fun.each(function(_, rbl)
    local to_resolve = havegot['helo'] .. '.' .. rbl['rbl']
    gen_rbl_rule(to_resolve, rbl)
  end,
  fun.filter(function(_, rbl)
    if rbl['helo'] then return true end
    return false
  end, enabled_rbls))

  -- DKIM RBLs
  fun.each(function(_, rbl)
    for _, d in ipairs(havegot['dkim']) do
      if rbl['dkim_domainonly'] then
        d = rspamd_util.get_tld(d)
      end
      local to_resolve = d .. '.' .. rbl['rbl']
      gen_rbl_rule(to_resolve, rbl)
    end
  end,
  fun.filter(function(_, rbl)
    if rbl['dkim'] then return true end
    return false
  end, enabled_rbls))

  -- Emails RBLs
  fun.each(function(_, rbl)
    if rbl['emails'] == 'domain_only' then
      local cleanList = {}
      for _, email in ipairs(havegot['emails']) do
        cleanList[email:get_host()] = true
      end
      for k in pairs(cleanList) do
        local to_resolve
        if rbl['hash'] then
          to_resolve = make_hash(tostring(k), rbl['hash']) .. '.' .. rbl['rbl']
        else
          to_resolve = k .. '.' .. rbl['rbl']
        end
        gen_rbl_rule(to_resolve, rbl)
      end
    else
      for _, email in ipairs(havegot['emails']) do
        local to_resolve
        if rbl['hash'] then
          to_resolve = make_hash(email:get_user() .. '@' .. email:get_host(), rbl['hash']) .. '.' .. rbl['rbl']
        else
          local upart = email:get_user()
          if validate_dns(upart) then
            to_resolve = upart .. '.' .. email:get_host() .. '.' .. rbl['rbl']
          end
        end
        if to_resolve then
          gen_rbl_rule(to_resolve, rbl)
        end
      end
    end
  end,
  fun.filter(function(_, rbl)
    if rbl['emails'] then return true end
    return false
  end, enabled_rbls))

  -- RDNS lists
  fun.each(function(_, rbl)
    local to_resolve = havegot['rdns'] .. '.' .. rbl['rbl']
    gen_rbl_rule(to_resolve, rbl)
  end,
  fun.filter(function(_, rbl)
    if rbl['rdns'] then return true end
    return false
  end, enabled_rbls))

  -- From lists
  fun.each(function(_, rbl)
    if (havegot['from']:get_version() == 6 and rbl['ipv6']) or
      (havegot['from']:get_version() == 4 and rbl['ipv4']) then
      local to_resolve = ip_to_rbl(havegot['from'], rbl['rbl'])
      gen_rbl_rule(to_resolve, rbl)
    end
  end,
  fun.filter(function(_, rbl)
    if rbl['from'] then return true end
    return false
  end, enabled_rbls))

  -- Received lists
  fun.each(function(_, rbl)
    for _,rh in ipairs(havegot['received']) do
      if rh['real_ip'] and rh['real_ip']:is_valid() then
        if ((rh['real_ip']:get_version() == 6 and rbl['ipv6']) or
          (rh['real_ip']:get_version() == 4 and rbl['ipv4'])) and
          ((rbl['exclude_private_ips'] and not rh['real_ip']:is_local()) or
          not rbl['exclude_private_ips']) and ((rbl['exclude_local_ips'] and
          not is_excluded_ip(rh['real_ip'])) or not rbl['exclude_local_ips']) then
          -- Disable forced for received resolving, as we have no control on
          -- those headers count
          local to_resolve = ip_to_rbl(rh['real_ip'], rbl['rbl'])
          local rule = gen_rbl_rule(to_resolve, rbl)
          rule.forced = false
        end
      end
    end
  end,
  fun.filter(function(_, rbl)
    if rbl['received'] then return true end
    return false
  end, enabled_rbls))

  local r = task:get_resolver()
  for _,p in pairs(params) do
    r:resolve_a({
      task = task,
      name = p.to_resolve,
      callback = p.callback,
      forced = p.forced
    })
  end
end

-- Configuration
local opts = rspamd_config:get_all_opt(N)
if not (opts and type(opts) == 'table') then
  rspamd_logger.infox(rspamd_config, 'Module is unconfigured')
  return
end

-- Plugin defaults should not be changed - override these in config
-- New defaults should not alter behaviour
local default_defaults = {
  ['default_enabled'] = {[1] = true, [2] = 'enabled'},
  ['default_ipv4'] = {[1] = true, [2] = 'ipv4'},
  ['default_ipv6'] = {[1] = false, [2] = 'ipv6'},
  ['default_received'] = {[1] = true, [2] = 'received'},
  ['default_from'] = {[1] = false, [2] = 'from'},
  ['default_unknown'] = {[1] = false, [2] = 'unknown'},
  ['default_rdns'] = {[1] = false, [2] = 'rdns'},
  ['default_helo'] = {[1] = false, [2] = 'helo'},
  ['default_dkim'] = {[1] = false, [2] = 'dkim'},
  ['default_dkim_domainonly'] = {[1] = true, [2] = 'dkim_domainonly'},
  ['default_emails'] = {[1] = false, [2] = 'emails'},
  ['default_exclude_private_ips'] = {[1] = true, [2] = 'exclude_private_ips'},
  ['default_exclude_users'] = {[1] = false, [2] = 'exclude_users'},
  ['default_exclude_local'] = {[1] = true, [2] = 'exclude_local'},
  ['default_is_whitelist'] = {[1] = false, [2] = 'is_whitelist'},
  ['default_ignore_whitelist'] = {[1] = false, [2] = 'ignore_whitelists'},
}
for default, default_v in pairs(default_defaults) do
  if opts[default] == nil then
    opts[default] = default_v[1]
  end
end

if(opts['local_exclude_ip_map'] ~= nil) then
  local_exclusions = rspamd_map_add(N, 'local_exclude_ip_map', 'radix',
    'RBL exclusions map')
end

local white_symbols = {}
local black_symbols = {}
local need_dkim = false

local id = rspamd_config:register_symbol({
  type = 'callback',
  callback = rbl_cb,
  flags = 'empty,nice'
})

local is_monitored = {}
for key,rbl in pairs(opts['rbls']) do
  (function()
    if rbl['disabled'] then return end
    for default, default_v in pairs(default_defaults) do
      if(rbl[default_v[2]] == nil) then
        rbl[default_v[2]] = opts[default]
      end
    end
    if not rbl['enabled'] then return end
    if type(rbl['returncodes']) == 'table' then
      for s,_ in pairs(rbl['returncodes']) do
        if type(rspamd_config.get_api_version) ~= 'nil' then
          rspamd_config:register_symbol({
            name = s,
            parent = id,
            type = 'virtual'
          })

          if rbl['dkim'] then
            need_dkim = true
          end
          if(rbl['is_whitelist']) then
            if type(rbl['whitelist_exception']) == 'string' then
              if (rbl['whitelist_exception'] ~= s) then
                table.insert(white_symbols, s)
              end
            elseif type(rbl['whitelist_exception']) == 'table' then
              local foundException = false
              for _, e in pairs(rbl['whitelist_exception']) do
                if e == s then
                  foundException = true
                  break
                end
              end
              if not foundException then
                table.insert(white_symbols, s)
              end
            else
                table.insert(white_symbols, s)
            end
          else
            if rbl['ignore_whitelists'] == false then
              table.insert(black_symbols, s)
            end
          end
        end
      end
    end
    if not rbl['symbol'] and
      ((rbl['returncodes'] and rbl['unknown']) or
      (not rbl['returncodes'])) then
        rbl['symbol'] = key
    end
    if type(rspamd_config.get_api_version) ~= 'nil' and rbl['symbol'] then
      rspamd_config:register_symbol({
        name = rbl['symbol'],
        parent = id,
        type = 'virtual'
      })

      if rbl['dkim'] then
        need_dkim = true
      end
      if (rbl['is_whitelist']) then
            if type(rbl['whitelist_exception']) == 'string' then
              if (rbl['whitelist_exception'] ~= rbl['symbol']) then
                table.insert(white_symbols, rbl['symbol'])
              end
            elseif type(rbl['whitelist_exception']) == 'table' then
              local foundException = false
              for _, e in pairs(rbl['whitelist_exception']) do
                if e == rbl['symbol'] then
                  foundException = true
                  break
                end
              end
              if not foundException then
                table.insert(white_symbols, rbl['symbol'])
              end
            else
              table.insert(white_symbols, rbl['symbol'])
            end
      else
        if rbl['ignore_whitelists'] == false then
          table.insert(black_symbols, rbl['symbol'])
        end
      end
    end
    if rbl['rbl'] then
      if not rbl['disable_monitoring'] and not rbl['is_whitelist'] and not is_monitored[rbl['rbl']] then
        is_monitored[rbl['rbl']] = true
        rbl.monitored = rspamd_config:register_monitored(rbl['rbl'], 'dns',
          {
            rcode = 'nxdomain',
            prefix = rbl['monitored_address'] or default_monitored
          })
      end

      rbls[key] = rbl
    end
  end)()
end
for _, w in pairs(white_symbols) do
  for _, b in pairs(black_symbols) do
    local csymbol = 'RBL_COMPOSITE_' .. w .. '_' .. b
    rspamd_config:set_metric_symbol(csymbol, 0, 'Autogenerated composite')
    rspamd_config:add_composite(csymbol, w .. ' & ' .. b)
  end
end
if need_dkim then
  rspamd_config:register_dependency(id, symbols['dkim_allow_symbol'])
end
