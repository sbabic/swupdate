-- SPDX-FileCopyrightText: 2014 Stefano Babic <sbabic@denx.de>
--
-- SPDX-License-Identifier: CC0-1.0

require("swupdate")
require("fpga")

-- register handlers
-- this is the file that is loaded by swupdate
-- at the startup.
-- Each handler will have a line to register itself to the
-- system.
-- The file is serached in this order:
-- '/usr/share/lua/5.2/swupdate_handlers.lua'
-- '/usr/share/lua/5.2/swupdate_handlers/init.lua'
-- '/usr/lib/lua/5.2/swupdate_handlers.lua'
-- '/usr/lib/lua/5.2/swupdate_handlers/init.lua'
-- './swupdate_handlers.lua'
-- '/usr/lib/lua/5.2/swupdate_handlers.so'
-- '/usr/lib/lua/5.2/loadall.so'
-- './swupdate_handlers.so'
-- It is duty of the buildsystem to install the self written
-- Lua handler. This file is not automatically installed.
