--[[

    SWUpdate Suricatta Example Lua Module for the 'General Purpose HTTP Server'.

    Author: Christian Storm <christian.storm@siemens.com>
    Copyright (C) 2022, Siemens AG

    SPDX-License-Identifier: GPL-2.0-or-later
--]]

--luacheck: no max line length

local suricatta = require("suricatta")

--[[ >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> ]]
--[[ Library Functions                                                         ]]
--[[ <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< ]]

--- Simplistic getopt()
--
-- Only short options (one dash, one character) are supported.
-- Unknown options are (silently) ignored.
-- If an option's required argument is missing, (':', option) is returned.
--
--- @param  argv       table   Integer-keyed arguments table
--- @param  optstring  string  GETOPT(3)-like option string
--- @return function           # Iterator, returning the next (option, optarg) pair
function getopt(argv, optstring)
    if type(argv) ~= "table" or type(optstring) ~= "string" then
        return function() end
    end
    for index, value in ipairs(argv) do
        argv[value] = index
    end
    local f = string.gmatch(optstring, "%l:?")
    return function()
        while true do
            local option = f()
            if not option then
                return
            end
            local optchar = option:sub(1, 1)
            local param = "-" .. optchar
            if argv[param] then
                if option:sub(2, 2) ~= ":" then
                    return optchar, nil
                end
                local value = argv[argv[param] + 1]
                if not value then
                    return ":", optchar
                end
                if value:sub(1, 1) == "-" then
                    return ":", optchar
                end
                return optchar, value
            end
        end
    end
end


--- Merge two tables' data into one.
--
--- @param  dest    table  Destination table, modified
--- @param  source  table  Table to merge into dest, overruling `dest`'s data if existing
--- @return table          # Merged table
function table.merge(dest, source)
    local function istable(t)
        return type(t) == "table"
    end
    for k, v in pairs(source) do
        if istable(v) and istable(dest[k]) then
            table.merge(dest[k], v)
        else
            dest[k] = v
        end
    end
    return dest
end


--- Escape and trim a String.
--
-- The default substitutions table is suitable for escaping
-- to proper JSON.
--
--- @param  str      string  The JSON string to be escaped
--- @param  substs?  table   Substitutions to apply
--- @return string           # The escaped JSON string
function escape(str, substs)
    local escapes = '[%z\1-\31"\\]'
    if not substs then
        substs = {
            ['"'] = '"',
            ["\\"] = "\\\\",
            ["\b"] = "",
            ["\f"] = "",
            ["\n"] = "",
            ["\r"] = "",
            ["\t"] = "",
        }
    end
    substs.__index = function(_, c)
        return string.format("\\u00%02X", string.byte(c))
    end
    setmetatable(substs, substs)
    return ((string.gsub(str, escapes, substs)):match("^%s*(.-)%s*$"):gsub("%s+", " "))
end


--[[ >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> ]]
--[[ Suricatta General Purpose HTTP Server Module                              ]]
--[[ <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< ]]

--- Marker for not (yet) set valid Device ID
DEVICE_ID_INVALID = "InvalidDeviceID"

--- Device state and information.
--
--- @class device
--- @field id  string  Device ID
device = {
    id = DEVICE_ID_INVALID
}


--- Job type "enum".
--
--- @enum job.type
jobtype = {
    INSTALL = 1,
    DOWNLOAD = 2,
    BOTH = 3,
}


--- A Job.
--
--- @class job
--- @field md5  string | nil    MD5 sum of the artifact
--- @field url  string | nil    URL of the artifact
--- @field typ  job.type | nil  Type of job


--- Configuration for the General Purpose HTTP Server.
--
--- @class gs
--- @field channel         table<string, suricatta.open_channel>  Channels used in this module
--- @field channel_config  suricatta.channel.options              Channel options (defaults, shadowed by config file, shadowed by command line arguments)
--- @field job             job                                    Current job information
--- @field polldelay       table                                  Default and temporary delay between two server poll operations in seconds
gs = {
    channel        = {},
    channel_config = {},
    job            = {},
    polldelay      = { default = 0, current = 0 },
}


--- Query and handle pending actions on server.
--
-- Lua counterpart of `server_op_res_t server_has_pending_action(int *action_id)`.
--
-- The suricatta.status return values UPDATE_AVAILABLE and ID_REQUESTED
-- are handled in `suricatta/suricatta.c`, the others result in SWUpdate
-- sleeping again.
--
--- @param  action_id  number  Current Action ID [unused]
--- @return number|nil         # Action ID [optional]
--- @return suricatta.status   # Suricatta return code
function has_pending_action(action_id)
    action_id = action_id
    gs.polldelay.current = gs.polldelay.default
    if suricatta.pstate.get() == suricatta.pstate.INSTALLED then
        suricatta.notify.warn("An installed update is pending testing, not querying server.")
        return action_id, suricatta.status.NO_UPDATE_AVAILABLE
    end

    suricatta.notify.trace("Querying %q", gs.channel_config.url)
    local _, _, data = gs.channel.default.get({
        url = gs.channel_config.url,
        format = suricatta.channel.content.NONE,
        nocheckanswer = true, -- Don't print 404 ERROR() message, see comment below.
        headers_to_send = { ["name"] = device.id },
    })

    if data.http_response_code == 404 then
        -- Note: Returning 404 on no update available is a bit unfortunate
        -- printing an error message if `nocheckanswer` isn't true.
        -- Maybe '204 No Content' as not repurposing a general error condition
        -- would've been a better choice. Then, 204 has to be introduced in
        -- channel_map_http_code() as known though.
        if math.random(3) == 1 then
            -- Deliberately update the server from time to time with my health status.
            suricatta.notify.trace(
                "Server queries client data for: %s",
                data.received_headers["Served-Client"] or "<Unknown>"
            )
            return action_id, suricatta.status.ID_REQUESTED
        end
        suricatta.notify.trace("Server served request for: %s", data.received_headers["Served-Client"] or "<Unknown>")
        return action_id, suricatta.status.NO_UPDATE_AVAILABLE
    end

    if data.http_response_code == 503 then
        -- Server is busy serving updates to another client.
        -- Try again after seconds announced in HTTP header or default value.
        gs.polldelay.current = tonumber(data.received_headers["Retry-After"]) or gs.polldelay.default
        suricatta.notify.debug("Server busy, waiting for %ds.", gs.polldelay.current)
        return action_id, suricatta.status.NO_UPDATE_AVAILABLE
    end

    if data.http_response_code == 302 then
        -- Returning 302 is, like 404 above, also a bit unfortunate as it
        -- requires telling curl not to follow the redirection but instead
        -- treat this as the artifact's URL (see channel initialization
        -- in server_start()).
        suricatta.notify.info("Update available, update job enqueued.")
        gs.job.md5 = data.received_headers["Content-Md5"]
        gs.job.url = data.received_headers["Location"]
        return action_id, suricatta.status.UPDATE_AVAILABLE
    end

    suricatta.notify.trace("Unhandled HTTP status code %d.", data.http_response_code)
    return action_id, suricatta.status.NO_UPDATE_AVAILABLE
end
suricatta.server.register(has_pending_action, suricatta.server.HAS_PENDING_ACTION)


--- Callback to check for update cancellation on server while downloading.
--
-- Some servers, e.g., hawkBit, support (remote) update cancellation while
-- the download phase. This (optional) callback function is registered
-- as `channel_data_t`'s `dwlwrdata` function for this purpose.
--
-- Note: The CALLBACK_PROGRESS and CALLBACK_CHECK_CANCEL callback functions
-- are both executed under mutual exclusion in suricatta's Lua state which
-- is suspended in the call to suricatta.{install,download}(). While this is
-- safe, both callback functions should take care not to starve each other.
--
-- WARNING: This function is called as part of the CURLOPT_WRITEFUNCTION
-- callback as soon as there is data received, i.e., usually *MANY* times.
-- Since this function and CALLBACK_PROGRESS are contending for the Lua
-- state lock (and by extension for some others), register and use this
-- function only if really necessary.
--
--- @return suricatta.status  # OK to continue downloading, UPDATE_CANCELED to cancel
function check_cancel_callback()
    return suricatta.status.OK
end
-- suricatta.server.register(check_cancel_callback, suricatta.server.CALLBACK_CHECK_CANCEL)


--- Progress thread callback handling progress reporting to remote.
--
-- Deliberately just uploading the JSON content while not respecting
-- the log configuration of the real `server_general.c` implementation.
-- This callback function is optional.
--
-- Note: The CALLBACK_PROGRESS and CALLBACK_CHECK_CANCEL callback functions
-- are both executed under mutual exclusion in suricatta's Lua state which
-- is suspended in the call to suricatta.{install,download}(). While this is
-- safe, both callback functions should take care not to starve each other.
--
--- @param  message  suricatta.ipc.progress_msg  The progress message
--- @return suricatta.status                     # Suricatta return code
function progress_callback(message)
    if not gs.channel.progress then
        return suricatta.status.OK
    end
    local logmessage
    if message.dwl_percent > 0 and message.dwl_percent <= 100 and message.cur_step == 0 then
        -- Rate limit progress messages sent to server.
        if message.dwl_percent % 5 ~= 0 then
            return suricatta.status.OK
        end
        if gs.job.typ == jobtype.INSTALL then
            logmessage = escape(
                string.format([[{"message": "File Processing...", "percent": %d}]], message.dwl_percent or 0)
            )
        else
            logmessage = escape(
                string.format([[{"message": "Downloading...", "percent": %d}]], message.dwl_percent or 0)
            )
        end
    elseif message.dwl_percent == 100 and message.cur_step > 0 then
        -- Rate limit progress messages sent to server.
        if message.cur_percent % 5 ~= 0 then
            return suricatta.status.OK
        end
        logmessage = escape(
            string.format(
                [[{"message": "Installing artifact %d/%d: '%s' with '%s'...", "percent": %d}]],
                message.cur_step,
                message.nsteps or 0,
                message.cur_image or "<UNKNOWN>",
                message.hnd_name or "<UNKNOWN>",
                message.cur_percent or 0
            )
        )
    end
    if logmessage ~= nil then
        local res, _, data = gs.channel.progress.put({
            url = string.format("%s/%s", gs.channel_config.url, "log"),
            content_type = "application/json",
            method = suricatta.channel.method.PUT,
            format = suricatta.channel.content.NONE,
            request_body = logmessage,
        })
        if not res then
            suricatta.notify.error("HTTP Error %d while uploading log.", tonumber(data.http_response_code) or -1)
        end
    end
    return suricatta.status.OK
end
suricatta.server.register(progress_callback, suricatta.server.CALLBACK_PROGRESS)


--- Install an update.
--
-- Lua counterpart of `server_op_res_t server_install_update(void)`.
--
--- @return suricatta.status  # Suricatta return code
function install_update()
    local res

    suricatta.notify.info("Installing artifact from %q", gs.job.url)

    -- Open progress reporting channel with default options.
    res, gs.channel.progress = suricatta.channel.open({})
    if not res then
        suricatta.notify.error("Cannot initialize progress reporting channel, will not send progress.")
        gs.channel.progress = nil
    end

    -- Chose an installation mode as I please...
    -- Note: `drain_messages` is false, i.e., do not upload strictly all
    -- progress messages to not artificially lengthen the installation
    -- process due to progress message network I/O. Hence, only while
    -- the update operation is in flight, progress messages are offloaded.
    -- Once the operation has finished, the possibly remaining progress
    -- messages are discarded. Instead of all progress messages, send a
    -- final notification for the server's information.
    res = suricatta.status.OK
    local url = gs.job.url
    local destfile
    if math.random(2) == 2 then
        suricatta.notify.info(">> Running in download + installation mode.")
    else
        suricatta.notify.info(">> Running in download and then local installation mode.")
        -- Note: suricatta.get_tmpdir() returned path is '/' terminated.
        destfile = ("%s%s"):format(suricatta.get_tmpdir(), "update.swu")
        gs.job.typ = jobtype.DOWNLOAD
        res, _, _ = suricatta.download({ channel = gs.channel.default, url = url, drain_messages = false }, destfile)
        url = ("file://%s"):format(destfile)
        gs.job.typ = jobtype.INSTALL
        if not res then
            suricatta.notify.error("Error downloading artifact!")
        end
    end
    if res then
        if not gs.job.typ then
            gs.job.typ = jobtype.BOTH
        end
        res, _, _ = suricatta.install({ channel = gs.channel.default, url = url, drain_messages = false })
        if destfile then
            os.remove(destfile)
        end
    end

    if gs.channel.progress then
        local finres, _, findata = gs.channel.progress.put({
            url = string.format("%s/%s", gs.channel_config.url, "log"),
            content_type = "application/json",
            method = suricatta.channel.method.PUT,
            format = suricatta.channel.content.NONE,
            request_body = escape([[{"message": "Final Note: Installation done :)"}]]),
        })
        if not finres then
            suricatta.notify.error(
                "HTTP Error %d while uploading final notification log.",
                tonumber(findata.http_response_code) or -1
            )
        end
        gs.channel.progress.close()
        gs.channel.progress = nil
    end

    gs.job = {}

    if not res then
        suricatta.notify.error("Error installing artifact!")
        return suricatta.status.EERR
    end

    suricatta.notify.info("Update artifact installed successfully.")
    return suricatta.status.OK
end
suricatta.server.register(install_update, suricatta.server.INSTALL_UPDATE)


--- Print the help text.
--
-- Lua counterpart of `void server_print_help(void)`.
--
--- @param  defaults  suricatta.channel.options|table  Default options
--- @return suricatta.status                           # Suricatta return code
function print_help(defaults)
    defaults = defaults or {}
    local stdout = io.output()
    stdout:write(string.format("\t  -u <URL>          * URL to the server instance, e.g., http://localhost:8080\n"))
    stdout:write(string.format("\t  -i <string>       * The device ID.\n"))
    stdout:write(string.format("\t  -p <int>          Polling delay (default: %ds).\n", defaults.polldelay))
    return suricatta.status.OK
end
suricatta.server.register(print_help, suricatta.server.PRINT_HELP)


--- Start the Suricatta server.
--
-- Lua counterpart of `server_op_res_t server_start(char *fname, int argc, char *argv[])`.
--
--- @param  defaults  table<string, any>  Lua suricatta module channel default options
--- @param  argv      table[]             C's `argv` as Lua Table
--- @param  fconfig   table<string, any>  SWUpdate configuration file's [suricatta] section as Lua Table ∪ { polldelay = CHANNEL_DEFAULT_POLLING_INTERVAL }
--- @return suricatta.status  # Suricatta return code
function server_start(defaults, argv, fconfig)
    -- Use defaults,
    -- ... shadowed by configuration file values,
    -- ... shadowed by command line arguments.
    local configuration = defaults or {}
    table.merge(configuration, fconfig or {})
    device.id, configuration.id = configuration.id, nil
    gs.polldelay.default, configuration.polldelay = configuration.polldelay, nil
    gs.channel_config = configuration
    for opt, arg in getopt(argv or {}, "u:i:p:") do
        if opt == "u" then
            gs.channel_config.url = tostring(arg)
        elseif opt == "i" then
            device.id = tostring(arg)
        elseif opt == "p" then
            gs.polldelay.default = tonumber(arg)
        elseif opt == ":" then
            io.stderr:write("Missing argument.")
            -- Note: .polldelay is in fconfig
            print_help(setmetatable((defaults or {}), { __index = fconfig }))
            return suricatta.status.EINIT
        end
    end
    gs.polldelay.current = gs.polldelay.default

    if not gs.channel_config.url or device.id == DEVICE_ID_INVALID then
        suricatta.notify.error("Mandatory configuration parameter missing.")
        return suricatta.status.EINIT
    end

    local res
    res, gs.channel.default = suricatta.channel.open({ url = gs.channel_config.url, nofollow = true })
    if not res then
        suricatta.notify.error("Cannot initialize channel.")
        return suricatta.status.EINIT
    end

    suricatta.notify.info("Running with device ID %s on %q.", device.id, gs.channel_config.url)
    return suricatta.status.OK
end
suricatta.server.register(server_start, suricatta.server.SERVER_START)


--- Stop the Suricatta server.
--
-- Lua counterpart of `server_op_res_t server_stop(void)`.
--
--- @return suricatta.status  # Suricatta return code
function server_stop()
    gs.channel_config = {}
    for channel, _ in pairs(gs.channel) do
        gs.channel[channel].close()
        gs.channel[channel] = nil
    end
    return suricatta.status.OK
end
suricatta.server.register(server_stop, suricatta.server.SERVER_STOP)


--- Query the polling interval from remote.
--
-- Lua counterpart of `unsigned int server_get_polling_interval(void)`.
--
--- @return number  # Polling interval in seconds
function get_polling_interval()
    -- Not implemented at server side, hence return device-local polling
    -- interval that is configurable via IPC or the server-announced wait
    -- time after having received a 503: busy while serving another client.
    return gs.polldelay.current
end
suricatta.server.register(get_polling_interval, suricatta.server.GET_POLLING_INTERVAL)


--- Send device configuration/data to remote.
--
-- Lua counterpart of `server_op_res_t server_send_target_data(void)`.
--
--- @return suricatta.status  # Suricatta return code
function send_target_data()
    local res, _, data = gs.channel.default.put({
        url = string.format("%s/%s", gs.channel_config.url, "log"),
        content_type = "application/json",
        method = suricatta.channel.method.PUT,
        format = suricatta.channel.content.NONE,
        request_body = string.format([[{"message": "I'm %s and I'm fine."}]], tostring(device.id)),
    })
    if not res then
        suricatta.notify.error("HTTP Error %d while uploading target data.", tonumber(data.http_response_code) or -1)
    end
    return suricatta.status.OK
end
suricatta.server.register(send_target_data, suricatta.server.SEND_TARGET_DATA)


--- Handle IPC messages sent to Suricatta Lua module.
--
-- Lua counterpart of `server_op_res_t server_ipc(ipc_message *msg)`.
--
--- @param  message  suricatta.ipc.ipc_message  The IPC message sent
--- @return string                              # IPC JSON reply string
--- @return suricatta.status                    # Suricatta return code
function ipc(message)
    if not (message or {}).json then
        return escape([[{ "request": "IPC requests must be JSON formatted" }]], { ['"'] = '"' }),
            suricatta.status.EBADMSG
    end
    message.msg = message.msg or "<NONE>"
    if message.cmd == message.commands.CONFIG then
        suricatta.notify.debug("Got IPC configuration message: %s", message.msg)
        if message.json.polling then
            gs.polldelay.default = tonumber(message.json.polling) or gs.polldelay.default
            return escape([[{ "request": "applied" }]], { ['"'] = '"' }), suricatta.status.OK
        end
    elseif message.cmd == message.commands.ACTIVATION then
        suricatta.notify.debug("Got IPC activation message: %s", message.msg)
        return escape([[{ "request": "inapplicable" }]], { ['"'] = '"' }), suricatta.status.OK
    end
    suricatta.notify.warn("Got unknown IPC message: %s", message.msg)
    return escape([[{ "request": "unknown" }]], { ['"'] = '"' }), suricatta.status.EBADMSG
end
suricatta.server.register(ipc, suricatta.server.IPC)
