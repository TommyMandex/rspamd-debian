--[[
Copyright (c) 2011-2015, Vsevolod Stakhov <vsevolod@highsecure.ru>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
]]--

-- IP score is a module that set ip score of specific ip and 

-- Default settings
local keystorage_host = nil
local keystorage_port = 0
local metric = 'default'
local reject_score = 3
local add_header_score = 1
local no_action_score = -2
local symbol = 'IP_SCORE'
-- This score is used for normalization of scores from keystorage
local normalize_score = 100 
local whitelist = nil
local expire = 240
local rspamd_redis = require "rspamd_redis"
local rspamd_logger = require "rspamd_logger"

-- Set score based on metric's action
local ip_score_set = function(task)
	-- Callback generator
	local make_key_cb = function(ip)
		local cb = function(task, err, data)
			if err then
				if string.find(err, 'not found') then 
					-- Try to set this key
					local cb_set = function(task, err, data)
						if err then
							if err ~= 'OK' then
								rspamd_logger.info('got error: ' .. err)
							end
						end
					end
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb_set, 'SET %b %b %b', ip, expire, '0')
				else
					rspamd_logger.info('got error while incrementing: ' .. err)
				end
			end
		end
		return cb
	end
	local action = task:get_metric_action(metric)
	if action then
		-- Check whitelist 
		if whitelist then
			if task:get_from_ip():is_valid() and whitelist:get_key(task:get_from_ip()) then
				-- Address is whitelisted
				return
			end
		end
		-- Now check action
		if action == 'reject' then
			local ip = task:get_from_ip()
			if ip:is_valid() then
				local cb = make_key_cb(ip)
				if reject_score > 0 then
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'INCRBY %b %b', ip, reject_score)
				else
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'DECRBY %b %b', ip, -reject_score)
				end
			end
		elseif action == 'add header' then
			local ip = task:get_from_ip()
			if ip:is_valid() then
				local cb = make_key_cb(ip)
				if add_header_score > 0 then
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'INCRBY %b %b', ip, add_header_score)
				else
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'DECRBY %b %b', ip, -add_header_score)
				end
			end
		elseif action == 'no action' then
			local ip = task:get_from_ip()
			if ip:is_valid() then
				local cb = make_key_cb(ip)
				if no_action_score > 0 then
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'INCRBY %b %b', ip, no_action_score)
				else
					rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'DECRBY %b %b', ip, -no_action_score)
				end
			end
		end
	end
end

-- Check score for ip in keystorage
local ip_score_check = function(task)
	local cb = function(task, err, data)
		if err then
			-- Key is not found or error occured
			return
		elseif data then
			local score = tonumber(data)
			-- Normalize
			if score > 0 and score > normalize_score then
				score = 1
			elseif score < 0 and score < -normalize_score then
				score = -1
			else
				score = score / normalize_score
			end
			task:insert_result(symbol, score)
		end
	end
	local ip = task:get_from_ip()
	if ip:is_valid() then
		if whitelist then
			if whitelist:get_key(task:get_from_ip()) then
				-- Address is whitelisted
				return
			end
		end
		rspamd_redis.make_request(task, keystorage_host, keystorage_port, cb, 'GET %b', ip)
	end
end


-- Configuration options
local configure_ip_score_module = function()
	local opts =  rspamd_config:get_all_opt('ip_score')
	if opts then
		if opts['keystorage_host'] then
			keystorage_host = opts['keystorage_host']
		end
		if opts['keystorage_port'] then
			keystorage_port = opts['keystorage_port']
		end
		if opts['metric'] then
			metric = opts['metric']
		end
		if opts['reject_score'] then
			reject_score = opts['reject_score']
		end
		if opts['add_header_score'] then
			add_header_score = opts['add_header_score']
		end
		if opts['no_action_score'] then
			no_action_score = opts['no_action_score']
		end
		if opts['symbol'] then
			symbol = opts['symbol']
		end
		if opts['normalize_score'] then
			normalize_score = opts['normalize_score']
		end
		if opts['whitelist'] then
			whitelist = rspamd_config:add_radix_map(opts['whitelist'])
		end
		if opts['expire'] then
			expire = opts['expire']
		end
	end
end

-- Registration
if rspamd_config:get_api_version() >= 9 then
	rspamd_config:register_module_option('ip_score', 'keystorage_host', 'string')
	rspamd_config:register_module_option('ip_score', 'keystorage_port', 'uint')
	rspamd_config:register_module_option('ip_score', 'metric', 'string')
	rspamd_config:register_module_option('ip_score', 'reject_score', 'int')
	rspamd_config:register_module_option('ip_score', 'add_header_score', 'int')
	rspamd_config:register_module_option('ip_score', 'no_action_score', 'int')
	rspamd_config:register_module_option('ip_score', 'symbol', 'string')
	rspamd_config:register_module_option('ip_score', 'normalize_score', 'uint')
	rspamd_config:register_module_option('ip_score', 'whitelist', 'map')
	rspamd_config:register_module_option('ip_score', 'expire', 'uint')

	configure_ip_score_module()
	if keystorage_host and keystorage_port and normalize_score > 0 then
		-- Register ip_score module
		rspamd_config:register_symbol(symbol, 1.0, ip_score_check)
		rspamd_config:register_post_filter(ip_score_set)
	end
else
	rspamd_logger.err('cannot register module ip_score as it requires at least 9 version of lua API and rspamd >= 0.4.6')
end
