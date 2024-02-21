--[[

    SWUpdate Suricatta Lua Module.

    Interface specification for the Lua module provided by the
    "Suricatta Lua module" suricatta "server" (suricatta/server_lua.c).

    Author: Christian Storm <christian.storm@siemens.com>
    Copyright (C) 2022, Siemens AG

    SPDX-License-Identifier: GPL-2.0-or-later

--]]

---@diagnostic disable: missing-return
---@diagnostic disable: unused-local
-- luacheck: no max line length
-- luacheck: no unused args

-- Note: Definitions prefixed with `---` are not part of the SWUpdate functionality
-- exposed to the Lua realm but instead are `typedef`-alikes returned by a function
-- of the SWUpdate Suricatta Lua Module. Nonetheless, they are in the suricatta
-- "namespace" for avoiding name clashes and, not least, convenience.

--- SWUpdate Suricatta Binding.
--
--- @class suricatta
local suricatta = {}

--- @enum suricatta.status
--- Lua equivalent of `server_op_res_t` enum as in `include/util.h`.
suricatta.status = {
    OK                  = 0,
    EERR                = 1,
    EBADMSG             = 2,
    EINIT               = 3,
    EACCES              = 4,
    EAGAIN              = 5,
    UPDATE_AVAILABLE    = 6,
    NO_UPDATE_AVAILABLE = 7,
    UPDATE_CANCELED     = 8,
    ID_REQUESTED        = 9,
}


--- SWUpdate notify function bindings.
--
-- Translates to `notify(string.format(message, ...))`,
-- @see `corelib/lua_interface.c`
--
--- @class suricatta.notify
suricatta.notify = {
    --- @type fun(message: string, ...: any)
    error    = function(message, ...) end,
    --- @type fun(message: string, ...: any)
    trace    = function(message, ...) end,
    --- @type fun(message: string, ...: any)
    debug    = function(message, ...) end,
    --- @type fun(message: string, ...: any)
    info     = function(message, ...) end,
    --- @type fun(message: string, ...: any)
    warn     = function(message, ...) end,
    --- @type fun(message: string, ...: any)
    progress = function(message, ...) end,
}


--- SWUpdate's bootloader interface as in `include/bootloader.h`.
--
--- @class suricatta.bootloader
suricatta.bootloader = {
    --- @enum suricatta.bootloader.bootloaders
    --- Bootloaders supported by SWUpdate.
    bootloaders = {
        EBG   = "ebg",
        NONE  = "none",
        GRUB  = "grub",
        UBOOT = "uboot",
    },
}

--- Get currently set bootloader's name.
--
--- @return suricatta.bootloader.bootloaders | nil  # Name of currently set bootloader
suricatta.bootloader.get = function() end

--- Test whether bootloader `name` is currently set.
--
--- @param  name     suricatta.bootloader.bootloaders  Name of bootloader to test for being currently selected
--- @return boolean                                    # True if `name` is currently set bootloader, false otherwise
suricatta.bootloader.is = function(name) end

--- Operations on the currently set bootloader's environment.
--
--- @class suricatta.bootloader.env
suricatta.bootloader.env = {}

--- Get value of a bootloader environment variable.
--
--- @param  variable  string  Name of the bootloader environment variable to get value for
--- @return string | nil      # Value of the bootloader environment variable or nil if non-existent
suricatta.bootloader.env.get = function(variable) end

--- Set value of a bootloader environment variable.
--
--- @param  variable  string  Name of the bootloader environment variable to set
--- @param  value     string  Value to set the bootloader environment variable `variable` to
--- @return boolean | nil     # True on success, nil on error
suricatta.bootloader.env.set = function(variable, value) end

--- Drop a bootloader environment variable.
--
--- @param  variable  string  Name of the bootloader environment variable to drop
--- @return boolean | nil     # True on success, nil on error
suricatta.bootloader.env.unset = function(variable) end

--- Set multiple bootloader environment variables from local file.
--
--- @param  filename  string  Path to local file in format `<variable>=<value>`
--- @return boolean | nil     # True on success, nil on error
suricatta.bootloader.env.apply = function(filename) end


--- SWUpdate's persistent state IDs as in `include/state.h` and reverse-lookup.
--
--- @class suricatta.pstate
suricatta.pstate = {
    OK            = string.byte('0'), [string.byte('0')] = "OK",
    INSTALLED     = string.byte('1'), [string.byte('1')] = "INSTALLED",
    TESTING       = string.byte('2'), [string.byte('2')] = "TESTING",
    FAILED        = string.byte('3'), [string.byte('3')] = "FAILED",
    NOT_AVAILABLE = string.byte('4'), [string.byte('4')] = "NOT_AVAILABLE",
    ERROR         = string.byte('5'), [string.byte('5')] = "ERROR",
    WAIT          = string.byte('6'), [string.byte('6')] = "WAIT",
    IN_PROGRESS   = string.byte('7'), [string.byte('7')] = "IN_PROGRESS",
}

--- Get the current stored persistent state.
--
--- @return number   # Persistent state ID number, suricatta.pstate.ERROR if unsuccessful
suricatta.pstate.get = function() end

--- Save persistent state information.
--
--- @param  state  number  Persistent state ID number
--- @return boolean | nil  # True on success, nil on error
suricatta.pstate.save = function(state) end



--- Function registry IDs for Lua suricatta functions.
--
--- @class suricatta.server
suricatta.server = {
    HAS_PENDING_ACTION    = 0,
    INSTALL_UPDATE        = 1,
    SEND_TARGET_DATA      = 2,
    GET_POLLING_INTERVAL  = 3,
    SERVER_START          = 4,
    SERVER_STOP           = 5,
    IPC                   = 6,
    PRINT_HELP            = 7,
    CALLBACK_PROGRESS     = 8,
    CALLBACK_CHECK_CANCEL = 9,
}

--- Register a Lua function as Suricatta interface implementation.
--
--- @param  function_p  function   Function to register for `purpose`
--- @param  purpose     number     Suricatta interface function implemented (one of suricatta.server's enums)
--- @return boolean                # Whether operation was successful or not
suricatta.server.register = function(function_p, purpose) end


--- Channel result table.
--
-- Result in return of a channel operation.
--
--- @class suricatta.channel_operation_result
--- @field http_response_code  number
--- @field format              suricatta.channel.content
--- @field json_reply          table | nil   Table if `format` was `suricatta.channel.content.JSON`
--- @field raw_reply           string | nil  Table if `format` was `suricatta.channel.content.RAW`
--- @field received_headers    table<string, string> | nil


--- Options and methods on the opened channel.
--
-- Table as returned by `suricatta.channel.open()`.
--
--- @class suricatta.open_channel
--- @field options  suricatta.channel.options                                                                               Channel creation-time set options as in `include/channel_curl.h`.
--- @field get      fun(options: suricatta.channel.options): boolean, suricatta.status, suricatta.channel_operation_result  Channel get operation
--- @field put      fun(options: suricatta.channel.options): boolean, suricatta.status, suricatta.channel_operation_result  Channel put operation
--- @field close    fun()                                                                                                   Channel close operation

--- @class suricatta.channel
suricatta.channel = {

    -- Lua-alike of proxy environment variable usage marker as in `include/channel_curl.h`.
    -- An empty `proxy` string means to use proxy environment variables.
    -- @type string
    USE_PROXY_ENV = "",

    --- @enum suricatta.channel.content
    --- Content type passed over the channel as in `include/channel_curl.h`.
    content = {
        NONE = 0,
        JSON = 1,
        RAW  = 2,
    },

    --- @enum suricatta.channel.method
    --- Transfer method to use over channel as in `include/channel_curl.h`.
    method = {
        GET   = 0,
        POST  = 1,
        PUT   = 2,
        PATCH = 3,
    },

    --- Channel options as in `include/channel_curl.h`.
    --
    --- @class suricatta.channel.options
    --- @field url                 string | nil   `CURLOPT_URL` - URL for this transfer
    --- @field cached_file         string | nil   Resume download from cached file at path
    --- @field auth                string | nil   `CURLOPT_USERPWD` - user name and password to use in authentication
    --- @field request_body        string | nil   Data to send to server for `PUT` and `POST`
    --- @field iface               string | nil   `CURLOPT_INTERFACE` - source interface for outgoing traffic
    --- @field dry_run             boolean | nil  `swupdate_request`'s dry_run field as in `include/network_ipc.h`
    --- @field cafile              string | nil   `CURLOPT_CAINFO` - path to Certificate Authority (CA) bundle
    --- @field sslkey              string | nil   `CURLOPT_SSLKEY` - private key file for TLS and SSL client cert
    --- @field sslcert             string | nil   `CURLOPT_SSLCERT` - SSL client certificate
    --- @field ciphers             string | nil   `CURLOPT_SSL_CIPHER_LIST` - ciphers to use for TLS
    --- @field proxy               string | nil   `CURLOPT_PROXY` - proxy to use
    --- @field info                string | nil   `swupdate_request`'s info field as in `include/network_ipc.h`
    --- @field auth_token          string | nil   String appended to Header
    --- @field content_type        string | nil   `Content-Type:` and `Accept:` appended to Header
    --- @field retry_sleep         number | nil   Time to wait prior to retry and resume a download
    --- @field method              suricatta.channel.method | nil  Channel transfer method to use
    --- @field retries             number | nil   Maximal download attempt count
    --- @field low_speed_timeout   number | nil   `CURLOPT_LOW_SPEED_TIME` - low speed limit time period
    --- @field connection_timeout  number | nil   `CURLOPT_CONNECTTIMEOUT` - timeout for the connect phase
    --- @field format              suricatta.channel.content | nil  Content type passed over the channel
    --- @field debug               boolean | nil  Set channel debug logging
    --- @field usessl              boolean | nil  Enable SSL hash sum calculation
    --- @field strictssl           boolean | nil  `CURLOPT_SSL_VERIFYHOST` + `CURLOPT_SSL_VERIFYPEER`
    --- @field nocheckanswer       boolean | nil  Whether the reply is interpreted/logged and tried to be parsed
    --- @field nofollow            boolean | nil  `CURLOPT_FOLLOWLOCATION` - follow HTTP 3xx redirects
    --- @field max_download_speed  string | nil   `CURLOPT_MAX_RECV_SPEED_LARGE` - rate limit data download speed
    --- @field headers_to_send     table<string, string> | nil  Header to send
    options = {},

    --- Open a new channel.
    --
    --- @param  options  suricatta.channel.options  Channel default options overridable per operation
    --- @return boolean                             # Whether operation was successful or not
    --- @return suricatta.open_channel              # Options of and operations on the opened channel
    open = function(options) end,
}


--- Channel and Options Table to use for an operation.
--
-- Channel to use for the download / installation operation as returned by `suricatta.channel.open()`
-- plus channel options overriding the defaults per operation (@see suricatta.channel.options)
-- and specific options to the download / installation operation, e.g., `drain_messages`.
--
--- @class suricatta.operation_channel
--- @field channel          suricatta.open_channel           Channel table as returned by `suricatta.channel.open()`
--- @field drain_messages   boolean  | nil                   Whether to flush all progress messages or only those while in-flight operation (default)
--- @field ∈                suricatta.channel.options | nil  Channel options to override for this operation

--- Install an update artifact from remote server or local file.
--
-- If the protocol specified in Table `install_channel`'s `url` field is `file://`,
-- a local update artifact file is installed. If it is, e.g., `https://`, the
-- update artifact is downloaded *and* installed.
-- Note that this file is to be deleted, if applicable, from the Lua realm.
--
--- @see suricatta.download
--- @param  install_channel  suricatta.operation_channel  Channel to use for the download+installation operation
--- @return boolean                                       # Whether operation was successful or not
--- @return suricatta.status                              # Suricatta return code
--- @return table<number, string>                         # Error messages, if any
suricatta.install = function(install_channel) end

--- Download an update artifact from remote server.
--
-- `suricatta.download()` just downloads an update artifact from the remote server
-- without installing it. For later installation, call `suricatta.install()` with
-- an appropriate `install_channel` Table's `url` field.
--
--- @see suricatta.install
--- @param  download_channel  suricatta.operation_channel  Channel to use for the download operation
--- @param  localpath         string                       Path where to store the downloaded artifact to
--- @return boolean                                        # Whether operation was successful or not
--- @return suricatta.status                               # Suricatta return code
--- @return table<number, string>                          # Error messages, if any
suricatta.download = function(download_channel, localpath) end


--- Sleep for a number of seconds.
--
-- Call `SLEEP(3)` via C realm.
--
--- @param seconds number  # Number of seconds to sleep
suricatta.sleep = function(seconds) end


--- Get TMPDIR from SWUpdate.
--
-- @see `core/util.c` :: get_tmpdir()
--
--- @return string  # TMPDIR path
suricatta.get_tmpdir = function() end


--- SWUpdate version information.
--- @class suricatta.version
--- @field [1]         number  SWUpdate's version
--- @field [2]         number  SWUpdate's patch level
--- @field version     number  SWUpdate's version
--- @field patchlevel  number  SWUpdate's patch level

--- Get SWUpdate version.
--
--- @return suricatta.version  # Table with `version` and `patchlevel` fields
suricatta.getversion = function() end


--- SWUpdate IPC types and definitions.
--
--- @class suricatta.ipc
suricatta.ipc = {}

--- @enum suricatta.ipc.sourcetype
--- Lua equivalent of `sourcetype` as in `include/swupdate_status.h`.
suricatta.ipc.sourcetype = {
    SOURCE_UNKNOWN           = 0,
    SOURCE_WEBSERVER         = 1,
    SOURCE_SURICATTA         = 2,
    SOURCE_DOWNLOADER        = 3,
    SOURCE_LOCAL             = 4,
    SOURCE_CHUNKS_DOWNLOADER = 5
}

--- @enum suricatta.ipc.RECOVERY_STATUS
--- Lua equivalent of `RECOVERY_STATUS` as in `include/swupdate_status.h`.
suricatta.ipc.RECOVERY_STATUS = {
    IDLE       = 0,
    START      = 1,
    RUN        = 2,
    SUCCESS    = 3,
    FAILURE    = 4,
    DOWNLOAD   = 5,
    DONE       = 6,
    SUBPROCESS = 7,
    PROGRESS   = 8
}

--- @enum suricatta.ipc.progress_cause
--- Lua equivalent of `progress_cause_t` as in `include/progress_ipc.h`.
suricatta.ipc.progress_cause = {
    CAUSE_NONE        = 0,
    CAUSE_REBOOT_MODE = 1,
}

--- Lua-alike of `progress_msg` as in `include/progress_ipc.h`.
--
--- @class suricatta.ipc.progress_msg
--- @field magic        number                         SWUpdate IPC magic number
--- @field status       suricatta.ipc.RECOVERY_STATUS  Update status
--- @field dwl_percent  number                         Percent of downloaded data
--- @field nsteps       number                         Total steps count
--- @field cur_step     number                         Current step
--- @field cur_percent  number                         Percent in current step
--- @field cur_image    string                         Name of the current image to be installed (max: 256 chars)
--- @field hnd_name     string                         Name of the running handler (max: 64 chars)
--- @field source       suricatta.ipc.sourcetype       The source that has triggered the update
--- @field info         string                         Additional information about the installation (max: 2048 chars)
--- @field jsoninfo     table                          If `info` is JSON, according Lua Table

--- Lua enum of IPC commands as in `include/network_ipc.h`.
--
-- `CMD_ENABLE` is not passed through and hence not in `ipc_commands`
-- as it's handled directly in `suricatta/suricatta.c`.
--
--- @type  {[string]: number}
--- @class suricatta.ipc.ipc_commands
--- @field ACTIVATION  number  0
--- @field CONFIG      number  1
--- @field GET_STATUS  number  3

--- Lua-alike of `ipc_message` as in `include/network_ipc.h`.
--
-- Note: Some members are deliberately not passed through to the Lua
-- realm such as `ipc_message.data.len` since that's transparently
-- handled by the C-to-Lua bridge.
-- Note: This is not a direct equivalent but rather a "sensible" selection
-- as, e.g., the `json` field is not present in `struct ipc_message`.
--
--- @class suricatta.ipc.ipc_message
--- @field magic     number                      SWUpdate IPC magic number
--- @field commands  suricatta.ipc.ipc_commands  IPC commands
--- @field cmd       number                      Command number, one of `ipc_commands`'s values
--- @field msg       string                      String data sent via IPC
--- @field json      table                       If `msg` is JSON, JSON as Lua Table


return suricatta
