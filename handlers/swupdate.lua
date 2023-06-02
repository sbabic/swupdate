--[[

    SWUpdate Lua Handler Interface.

    Interface specification for the Lua module
    provided by SWUpdate to Lua Handlers.
    See: corelib/lua_interface.c

    Copyright (C) 2022, Siemens AG
    Author: Christian Storm <christian.storm@siemens.com>

    SPDX-License-Identifier: GPL-2.0-or-later

--]]

---@diagnostic disable: missing-return
---@diagnostic disable: unused-local
-- luacheck: no unused args


--- SWUpdate Lua Handler Module.
--- @class swupdate
local swupdate = {}


--- @enum swupdate.RECOVERY_STATUS
--- Lua equivalent of `RECOVERY_STATUS` as in `include/swupdate_status.h`.
swupdate.RECOVERY_STATUS = {
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

--- Lua equivalent of `ERROR(format, ...)`.
--- @param format  string  Format string
--- @param ...     any     Varargs as referenced in format string
swupdate.error = function(format, ...) end

--- Lua equivalent of `TRACE(format, ...)`.
--- @param format  string  Format string
--- @param ...     any     Varargs as referenced in format string
swupdate.trace = function(format, ...) end

--- Lua equivalent of `INFO(format, ...)`.
--- @param format  string  Format string
--- @param ...     any     Varargs as referenced in format string
swupdate.info  = function(format, ...) end

--- Lua equivalent of `WARN(format, ...)`.
--- @param format  string  Format string
--- @param ...     any     Varargs as referenced in format string
swupdate.warn  = function(format, ...) end

--- Lua equivalent of `DEBUG(format, ...)`.
--- @param format  string  Format string
--- @param ...     any     Varargs as referenced in format string
swupdate.debug = function(format, ...) end

--- Lua equivalent of `notify(PROGRESS, ..., msg)`.
--- @param msg  string  Message to send to progress interface
swupdate.progress = function(msg) end

--- Lua equivalent of `notify(status, error, INFOLEVEL, msg)`.
--- @param status  swupdate.RECOVERY_STATUS  Current status, one of `swupdate.RECOVERY_STATUS`'s values
--- @param error   number                    Error code
--- @param msg     string                    Message
swupdate.notify = function(status, error, msg) end


--- Update progress.
--
--- @param percent  number  Progress percent to set
swupdate.progress_update = function(percent) end


--- Mount a filesystem to a temporary mountpoint.
--
--- @param  device      string  Device to mount
--- @param  filesystem  string  Device's filesystem
--- @return string              # Mountpoint for use with `swupdate.umount(target)`
swupdate.mount = function(device, filesystem) end

--- Unmount a mountpoint.
--
--- @param  target  string  Mountpoint to unmount
--- @return boolean | nil   # true if successful, nil on error
swupdate.umount = function(target) end


--- @enum swupdate.ROOT_DEVICE
--- Lua equivalent of `root_dev_type` as in `include/lua_util.h`.
swupdate.ROOT_DEVICE = {
    PATH      = 0,
    UUID      = 1,
    PARTUUID  = 2,
    PARTLABEL = 3
}


--- Current root device information.
--- @class swupdate.rootdev
--- @field type   swupdate.ROOT_DEVICE  Root device type, one of `swupdate.ROOT_DEVICE`'s values
--- @field value  string                Root device path
--- @field path   string                Full root device path, if identified, else nil

--- Get current root device.
--
--- @return swupdate.rootdev  # Table containing type, and (full) path to root device
swupdate.getroot = function() end


--- Table with major/minor numbers of the device on which the swupdate.stat()'d file resides.
--- @class swupdate.stat_dev
--- @field major  number  Major device number
--- @field minor  number  Minor device number

--- `struct stat`-alike Table.
--- @class swupdate.stat_info
--- @field mode          "directory"|"named pipe"|"link"|"regular file"|"socket"|"block device"|"char device"|"unknown"  File type and mode
--- @field dev           swupdate.stat_dev  ID of device containing file
--- @field ino           number             Inode number
--- @field nlink         number             Number of hard links
--- @field uid           number             User ID of owner
--- @field gid           number             Group ID of owner
--- @field rdev          swupdate.stat_dev  Device ID (if special file)
--- @field access        string             Time of last access, e.g., "Wed Jun 30 21:49:08 1993"
--- @field modification  string             Time of last modification, e.g., "Wed Jun 30 21:49:08 1993"
--- @field change        string             Time of last status change, e.g., "Wed Jun 30 21:49:08 1993"
--- @field size          number             Total size, in bytes
--- @field permissions   string             Unix file permissions string, e.g., "rwxr-xr-x"
--- @field blocks        number             Number of 512B blocks allocated
--- @field blksize       number             Block size for filesystem I/O

--- STAT(2) Wrapper.
--
--- @param  pathname  string          Retrieve information about the file `pathname`
--- @return swupdate.stat_info | nil  # `struct stat`-alike Table if successful, nil on error
swupdate.stat = function(pathname) end


--- Get SWUpdate's TMPDIRSCRIPT directory.
--
--- @return string  # TMPDIRSCRIPT directory
swupdate.tmpdirscripts = function() end

--- Get SWUpdate's TMPDIR directory.
--
--- @return string  # TMPDIR directory
swupdate.tmpdir = function() end


--- SWUpdate hardware information.
--- @class swupdate.hardware
--- @field boardname   string  SWUpdate's boardname
--- @field revision    string  SWUpdate's revision

--- Get SWUpdate hardware.
--
--- @return swupdate.hardware  # Table with 'boardname' and 'revision' fields
swupdate.get_hw = function() end


--- SWUpdate version information.
--- @class swupdate.version
--- @field [1]         number  SWUpdate's version
--- @field [2]         number  SWUpdate's patch level
--- @field version     number  SWUpdate's version
--- @field patchlevel  number  SWUpdate's patch level

--- Get SWUpdate version.
--
--- @return swupdate.version  # Table with 'version' and 'patchlevel' fields
swupdate.getversion = function() end


--- Get software Selection and Mode.
--
--- @return string  # Selection
--- @return string  # Mode
swupdate.get_selection = function() end


--- Set Bootloader environment key=value.
--
--- @param key    string  Bootloader environment key to set
--- @param value  string  Value to set `key` to in bootloader environment
swupdate.set_bootenv = function(key, value) end

--- Get Bootloader environment key's value.
--
--- @param  key  string  Bootloader environment's key to get value from
--- @return string       # Bootloader environment key's value, empty if key absent
swupdate.get_bootenv = function(key) end


--- @enum swupdate.HANDLER_MASK
--- Lua equivalent of `HANDLER_MASK` as in `include/handler.h`.
swupdate.HANDLER_MASK = {
    IMAGE_HANDLER      = 1,
    FILE_HANDLER       = 2,
    SCRIPT_HANDLER     = 4,
    BOOTLOADER_HANDLER = 8,
    PARTITION_HANDLER  = 16,
    NO_DATA_HANDLER    = 32,
    ANY_HANDLER        = 1 + 2 + 4 + 8 + 16 + 32
}

--- Register a Lua function as Handler implementation.
--
-- The signature of the function to be registered as Handler implementation is:
--
-- --- @param  image     img_type  Lua equivalent of `struct img_type`
-- --- @param  fn | nil  string    `preinst` or `postinst` for `SCRIPT_HANDLER`s, else nil
-- --- @return number    # 0 on success, 1 on error
-- function lua_handler(image, fn)
--     ...
-- end
--
--- @param name     string    Registered Handler's name
--- @param funcptr  function  Function to register as Handler implementation
--- @param mask     number    Type(s) for which to register the Handler, one or more ORed values of `swupdate.HANDLER_MASK`
swupdate.register_handler = function(name, funcptr, mask) end


--- Lua equivalent of `struct img_type {...}` as in `include/swupdate.h` plus `read()` and `copy2file()` functions.
--- @class img_type
--- @field name                  string    `sw-component` name to check with `sw-versions`
--- @field version               string    `sw-component` version to check with `sw-versions`
--- @field filename              string    File name in CPIO archive
--- @field volume                string    If handler is "ubivol", the UBI volume name
--- @field type                  string    The Handler name
--- @field device                string    Device node to operate on for mounting, flashing, ...
--- @field path                  string    For FILE_HANDLERs: the relative destination path
--- @field mtdname               string    MTD device where image must be installed
--- @field data                  string    Handler-specific arbitrary data
--- @field filesystem            string    For FILE_HANDLERs: the filesystem to mount
--- @field ivt                   string    IVT of an encrypted artifact
--- @field installed_directly    boolean   Whether to stream image to `device` w/o temporary copy
--- @field install_if_different  boolean   Whether to compare `name` and `version` accordingly
--- @field install_if_higher     boolean   Whether to compare `name` and `version` accordingly
--- @field encrypted             boolean   Whether the artifact is encrypted
--- @field partition             boolean   Whether the artifact is a partitioner
--- @field script                boolean   Whether the artifact is a script
--- @field preserve_attributes   boolean   Whether to preserve attributes in archives
--- @field offset                number    Offset to seek to in artifact
--- @field size                  number    Artifact size
--- @field checksum              number    Computed checksum
--- @field skip                  number    `skip_t` enum number as in `include/swupdate.h`
--- @field compressed            string    `zlib` or `zstd` (boolean value is deprecated)
--- @field properties            table     Properties Table equivalent as specified in `sw-description`
--- @field sha256                string    sha256 hash of the image, file, or script
local img_type = {
    --- Store the current image artifact to disk.
    --
    --- @param  self  img_type  This `img_type` instance
    --- @param  path  string    Path to store the current image artifact to
    --- @return number          # 0 on success, -1 on error
    --- @return string | nil    # nil on success, error message on failure
    ['copy2file'] = function(self, path) end,

    --- Process the current image artifact in Lua.
    --
    -- The `callback = function(chunk) ... end` is repeatedly called with
    -- chunked artifact data of type `string` in its `chunk` parameter as
    -- long as there is data available.
    -- The callback function must completely consume the artifact data so
    -- that SWUpdate can continue with the stream's next artifact after
    -- the Lua Handler returns.
    --
    --- @param  self      img_type  This `img_type` instance
    --- @param  callback  function  Callback `function(chunk) ... end` that is fed the current image artifact in chunks.
    --- @return number              # 0 on success, -1 on error
    --- @return string | nil        # nil on success, error message on failure
    ['read'] = function(self, callback) end,
}


--- @class swupdate.handler
--- Chain-callable SWUpdate Handlers (Non-exhaustive).
--
-- Note: This list of built-in Handlers is non-exhaustive and
-- purely illustrative. At run-time, SWUpdate populates this
-- Table with the actually available Handlers according to its
-- compile-time configuration and Lua Handlers loaded.
--
--- @type table<string, number>
swupdate.handler = {
    --- Archive Handler (See: `handlers/archive_handler.c`).
    ["archive"]        = 1,
    --- `tar` Archive Handler (See: `handlers/archive_handler.c`).
    ["tar"]            = 1,
    --- Bootloader Handler (See: `handlers/boot_handler.c`).
    ["bootloader"]     = 1,
    --- "dummy" Handler (See: `handlers/dummy_handler.c`).
    ["dummy"]          = 1,
    --- Delta Handler (See: `handlers/delta_handler.c`).
    ["delta"]          = 1,
    --- Disk Format Handler (See: `handlers/diskformat_handler.c`).
    ["diskformat"]     = 1,
    --- Disk Partition Handler (See: `handlers/diskpart_handler.c`).
    ["diskpart"]       = 1,
    --- Toggle Boot Flag Handler (See: `handlers/diskpart_handler.c`).
    ["toggleboot"]     = 1,
    --- Flash Hamming1 Handler (See: `handlers/flash_hamming1_handler.c`).
    ["flash-hamming1"] = 1,
    --- Flash Handler (See: `handlers/flash_handler.c`).
    ["flash"]          = 1,
    --- Raw Image Handler (See: `handlers/raw_handler.c`).
    ["raw"]            = 1,
    --- Raw File Handler (See: `handlers/raw_handler.c`).
    ["rawfile"]        = 1,
    --- Raw Copy Handler (See: `handlers/raw_handler.c`).
    ["rawcopy"]        = 1,
    --- rdiff Image Handler (See: `handlers/rdiff_handler.c`).
    ["rdiff_image"]    = 1,
    --- rdiff File Handler (See: `handlers/rdiff_handler.c`).
    ["rdiff_file"]     = 1,
    --- Readback Handler (See: `handlers/readback_handler.c`).
    ["readback"]       = 1,
    --- Remote Handler (See: `handlers/remote_handler.c`).
    ["remote"]         = 1,
    --- SSBL Handler (See: `handlers/ssbl_handler.c`).
    ["ssblswitch"]     = 1,
    --- SWU Forward Handler (See: `handlers/swuforward_handler.c`).
    ["swuforward"]     = 1,
    --- UBI Volume Handler (See: `handlers/ubivol_handler.c`).
    ["ubivol"]         = 1,
    --- UBI Partition Handler (See: `handlers/ubivol_handler.c`).
    ["ubipartition"]   = 1,
    --- UBI Swap Handler (See: `handlers/ubivol_handler.c`).
    ["ubiswap"]        = 1,
    --- ucfw Handler (See: `handlers/ucfw_handler.c`).
    ["ucfw"]           = 1,
    --- Unique UUID Handler (See: `handlers/uniqueuuid_handler.c`).
    ["uniqueuuid"]     = 1,
}

--- Chain-call another Handler.
--
--- @param  handler  string    Chain-called Handler's name
--- @param  image    img_type  Lua equivalent of `struct img_type` as in `include/swupdate.h`
--- @return number             # 0 on success, 1 on failure
--- @return string | nil       # nil on success, error message on failure
swupdate.call_handler = function(handler, image) end


return swupdate
