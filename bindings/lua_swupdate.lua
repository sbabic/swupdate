--[[

    SWUpdate IPC Lua Module Interface.

    Interface specification for the Lua IPC module.
    See: bindings/lua_swupdate.c

    Copyright (C) 2022, Siemens AG
    Author: Christian Storm <christian.storm@siemens.com>

    SPDX-License-Identifier: GPL-2.0-or-later

--]]

---@diagnostic disable: missing-return
---@diagnostic disable: unused-local
-- luacheck: no unused args


--- SWUpdate IPC Lua Module.
--- @class lua_swupdate
local lua_swupdate = {}


--- Get local IPv4 network interface(s) information.
--
-- The returned Table contains the network interface names
-- as keys and a space-separated IP address and subnet mask
-- as values, e.g., {['eth0']="192.168.1.1 255.255.255.0"}.
--
--- @return table<string, string>
lua_swupdate.ipv4 = function() end


--- @enum lua_swupdate.RECOVERY_STATUS
--- Lua equivalent of `RECOVERY_STATUS` as in `include/swupdate_status.h`.
lua_swupdate.RECOVERY_STATUS = {
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


--- @enum lua_swupdate.sourcetype
--- Lua equivalent of `sourcetype` as in `include/swupdate_status.h`.
lua_swupdate.sourcetype = {
    SOURCE_UNKNOWN           = 0,
    SOURCE_WEBSERVER         = 1,
    SOURCE_SURICATTA         = 2,
    SOURCE_DOWNLOADER        = 3,
    SOURCE_LOCAL             = 4,
    SOURCE_CHUNKS_DOWNLOADER = 5
}


--- Lua equivalent of `struct progress_msg` as in `include/progress_ipc.h`.
--- @class lua_swupdate.progress_msg
--- @field status    lua_swupdate.RECOVERY_STATUS  Update status, one of `lua_swupdate.RECOVERY_STATUS`'s values
--- @field download  number  Downloaded data percentage
--- @field source    lua_swupdate.sourcetype  Interface that triggered the update, one of `lua_swupdate.sourcetype`'s values
--- @field nsteps    number  Total number of steps
--- @field step      number  Current step
--- @field percent   number  Percentage done in current step
--- @field artifact  string  Name of image to be installed
--- @field handler   string  Name of running Handler
--- @field info      string  Additional information about installation


--- SWUpdate progress socket binding.
--
-- The returned Class Table contains methods to
-- interact with SWUpdate's progress socket.
--
lua_swupdate.progress = function()
    return {
        --- Connect to SWUpdate's progress socket.
        --
        --- @param  self  table   This `lua_swupdate.progress` instance
        --- @return number | nil  # The connection handle or nil in case of error
        --- @return nil | string  # nil or an error message in case of error
        connect = function(self) end,

        --- Receive data from SWUpdate's progress socket.
        --
        --- @param  self  table                        This `lua_swupdate.progress` instance
        --- @return table | lua_swupdate.progress_msg  # This `lua_swupdate.progress` instance on error or the received progress message
        --- @return nil                                # nil in case of error
        receive = function(self) end,

        --- Close connection to SWUpdate's progress socket.
        --
        --- @param  self  table  This `lua_swupdate.progress` instance
        --- @return table        # This `lua_swupdate.progress` instance
        close = function(self) end,
    }
end


--- SWUpdate control socket binding.
--
-- The returned Class Table contains methods to
-- interact with SWUpdate's control socket.
--
lua_swupdate.control = function()
    return {
        --- Connect to SWUpdate's control socket.
        --
        --- @param  self  table   This `lua_swupdate.control` instance
        --- @return number | nil  # Connection handle or nil in case of error
        --- @return nil | string  # nil or an error message in case of error
        connect = function(self) end,

        --- Write to connected SWUpdate control socket.
        --
        --- @param  self  table    This `lua_swupdate.control` instance
        --- @param  data  string   Chunk data to write to SWUpdate's control socket
        --- @return boolean | nil  # true or nil in case of error
        --- @return nil | string   # nil or an error message in case of error
        write = function(self, data) end,

        --- Close connection to SWUpdate's control socket.
        --
        --- @param  self  table    This `lua_swupdate.control` instance
        --- @return boolean | nil  # true or nil in case of error
        --- @return nil | string   # nil or an error message in case of error
        close = function(self) end,
    }
end


return lua_swupdate
