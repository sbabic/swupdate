--[[

    wfx SWUpdate Suricatta Lua Module.

    Author: Christian Storm <christian.storm@siemens.com>
    Copyright ©️ 2023, Siemens AG

    SPDX-License-Identifier: GPL-2.0-or-later

--]]

-- luacheck: no max line length
-- luacheck: no global

suricatta = require("suricatta")

-- Cater for table.unpack() on Lua 5.1 / LuaJIT
if not table.pack then
    ---@diagnostic disable-next-line: duplicate-set-field
    table.pack = function(...)
        return { n = select("#", ...), ... }
    end
    ---@diagnostic disable-next-line: deprecated
    table.unpack = unpack
end

--- Exported wfx SWUpdate Suricatta Lua Module.
local M = {
    --- Utility / Library Functions.
    utils = {
        --- Table Utility Functions.
        table = {},
        --- String Utility Functions.
        string = {},
    },
    --- DeviceArtifactUpdate Definitions and Functions.
    dau = {},
    --- Suricatta Interface Function Implementations.
    suricatta_funcs = {},
}

-- ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
-- ▏
-- ▏  Utility / Library Functions
-- ▏
-- ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔
--- @enum GETOPT_ARG
--- getopt() argument specifier enum.
M.utils.GETOPT_ARG = {
    NO = 0,
    REQUIRED = 1,
    [0] = "",
    [1] = ":",
}

--- Simplistic getopt(3)-alike.
--
-- The following getopt(3) features are not supported:
-- * multiple identical short option arguments (e.g. `-v -v`),
-- * optional arguments, e.g., `-v [level]`,
-- * non-option arguments.
-- Unknown options given are (silently) ignored.
--
-- The `opts` option table has the following format:
-- {
--   {"<longopt>", GETOPT_ARG.{NO,REQUIRED}, "<shortopt>" },
--   ...
-- }
-- `<longopt>` may be `nil` in which case only `<shortopt>` is recognized.
--
--- @param  argv  table  Integer-keyed arguments Table, modified in-place.
--- @param  opts  table  Option Table
--- @return function     # Iterator, returning the next (option, optarg) pair
function M.utils.getopt(argv, opts)
    local optmap = {}
    local optstring = ""
    for _, optdef in ipairs(opts) do
        assert(optdef[3], "Mandatory short option char absent!")
        assert(optdef[2], "Mandatory has_arg definition absent!")
        optstring = ("%s%s%s"):format(optstring, optdef[3], M.utils.GETOPT_ARG[optdef[2]])
        optmap[optdef[1] or "_"] = optdef[3]
    end
    for index, value in ipairs(argv) do
        if value:sub(1, 2) == "--" and optmap[value:sub(3)] then
            local optvalue = "-" .. optmap[value:sub(3)]
            argv[index] = optvalue
            argv[optvalue] = index
        elseif value:sub(1, 1) == "-" then
            argv[value] = index
        end
    end
    local f = optstring:gmatch("%a:?")
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

--- Reverse ipairs() iterator.
--
-- Like Lua's internal `ipairs()`, just reverse.
--
--- @param  tab  table  Table to iterate over in reverse ipairs()-order
--- @return function    # The iterator function
--- @return table       # The table `tab`
--- @return number      # Array index
function M.utils.ripairs(tab)
    local function rev_iter_ipairs(t, i)
        i = i - 1
        if i ~= 0 then
            return i, t[i]
        end
    end
    return rev_iter_ipairs, tab, #tab + 1
end

--- ipairs() + filter iterator.
--
--- @param  tab  table             Table to iterate over
--- @param  fun  fun(any):boolean  Filter function; evaluates to `true` to filter out item
--- @return function               # The iterator function
--- @return table                  # The table tab
--- @return number                 # Array index
function M.utils.ipairsf(tab, fun)
    local function iter_ipairs(t, i)
        while true do
            i = i + 1
            local v = t[i]
            if v == nil then
                return
            end
            if not fun(v) then
                return i, v
            end
        end
    end
    return iter_ipairs, tab, 0
end

--- Merge two Key=Value Tables, leftwards.
--
--- @param  tab  table  Destination Table, modified
--- @param  ovr  table  Table to merge into `tab`, overruling `tab`'s data if existing in `ovr`
--- @return table       # Merged Table, i.e., `tab`
function M.utils.table.merge(tab, ovr)
    local function istable(t)
        return type(t) == "table"
    end
    for k, v in pairs(ovr) do
        if istable(v) and istable(tab[k]) then
            M.utils.table.merge(tab[k], v)
        else
            tab[k] = v
        end
    end
    return tab
end

--- Check if a Table contains `value`.
--
--- @param  tab    table  Table to check for `value`
--- @param  value  any    Value to check for presence in `tab`
--- @return boolean       # `true` if `tab` contains `value`, `false` otherwise
function M.utils.table.contains(tab, value)
    if tab[value] then
        return true
    end
    for _, v in ipairs(tab) do
        if v == value then
            return true
        end
    end
    return false
end

--- Filter Array Table.
--- @param  tbl   table             Table to filter
--- @param  func  fun(any):boolean  Filter function; evaluates to `true` to filter out item
--- @return table                   # New table, filtered
function M.utils.table.ifilter(tbl, func)
    local newtbl = {}
    for _, v in ipairs(tbl or {}) do
        if not func(v) then
            newtbl[#newtbl + 1] = v
        end
    end
    return newtbl
end

--- Test if Table is empty.
--
--- @param  tbl  table  Table to test for emptiness
--- @return boolean     # `true` if empty, `false` if not
function M.utils.table.is_empty(tbl)
    local index, _ = next(tbl)
    return not index and true or false
end

--- Enquote a String array's elements.
--
--- @param  t  string[]  String array to quote elements of
--- @return string[]     # Element-quoted string array
function M.utils.string.enquote(t)
    local quoted = {}
    for _, v in ipairs(t) do
        quoted[#quoted + 1] = ('"%s"'):format(v)
    end
    return quoted
end

--- Escape and trim a String.
--
-- The default substitutions table is suitable for escaping
-- to proper JSON.
--
--- @param  str      string  The JSON string to be escaped
--- @param  substs?  table   Substitutions to apply (optional)
--- @return string           # The escaped JSON string
function M.utils.string.escape(str, substs)
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
        return ("\\u00%02X"):format(c:byte())
    end
    setmetatable(substs, substs)
    return str:gsub(escapes, substs):match("^%s*(.-)%s*$"):gsub("%s+", " ")
end

--- Execute a callback with retries and pause in between.
--
-- The `callback` function must return `true` or, in case of error,
-- `false` to attempt the next try (if any left).
--
--- @param  times     number                 Maximal number of tries
--- @param  pause     number                 Pause in seconds between tries (defaults to 1)
--- @param  callback  fun(...):boolean, ...  The callback function to try
--- @param  ...       any                    The callback function's parameters
--- @return boolean                          # `true`, or, in case of error, `false`
--- @return ...                              # Return values of `callback(...)`
function M.utils.do_retry(times, pause, callback, ...)
    local ret
    times, pause = times or 1, pause or 1
    for try = 1, times do
        ret = table.pack(callback(...))
        if ret[1] == true then
            return table.unpack(ret)
        end
        suricatta.notify.warn("Retry attempt %d/%d failed, sleeping %s seconds.", try, times, pause)
        suricatta.sleep(pause)
    end
    suricatta.notify.error("All %d retry attempts failed.", times)
    return table.unpack(ret)
end

--- Validate a JSON document against its schema.
--
-- A schema for a JSON document is defined in terms of Lua code,
-- given in the `schema` parameter, and validated against this.
--
-- The schema definition for a particular JSON key `jkey` is a Lua
-- Table specifying the `type` of `jkey`'s value, possibly extended
-- with `min` and `max` properties the value has to satisfy.
-- The latter properties are `type`-dependent: A property `max = 23`
-- for a number-typed `jkey` enforces its value to be less than or
-- equal to 23. For a string-typed `jkey`, a `max = 23` property
-- enforces the maximal string length to be less or equal to 23.
-- The `type` is simply given by an accordingly typed Lua value.
-- Per default, all keys (and their values) present in the schema
-- have to be present in the to be validated JSON document while
-- not necessarily vice versa. Optional schema JSON keys can be
-- flagged as such by the `optional = true` property.
-- *Note*: Any other extra fields (properties) in the JSON schema
-- are silently ignored.
--
-- For example,
--  `jkey = { type = "string", min = 1 }`
-- specifies `jkey` to be a string with length > 1.
--
-- As another example,
--  `jkey = { type = 100, max = 100, min = 0 }`
-- specifies `jkey` to be a number in [0,100].
--
-- This naturally extends to JSON arrays with its content schema
-- specified in the sole property `value` as follows
-- ```
--  jkey = {
--      type = {},
--      value = {
--          [1] = {
--              jkey = { type = "string", min = 1 }
--          },
--      },
--  }
-- ```
-- specifying a non-empty JSON array `jkey` with its array contents
-- being strings of minimal length 1.
--
--- @param  data    table   Data to validate against a schema
--- @param  schema  table   Schema table
--- @param  path?   string  Initial path
--- @return boolean         # `true` if `data` satisfies schema, `false` otherwise
function M.utils.validate_json_schema(data, schema, path)
    local function check_types(def, k, v, p)
        if not def[k] then
            if v.optional == true then
                if type(v.type) == "string" then
                    def[k] = "<unknown>"
                end
                if type(v.type) == "number" then
                    def[k] = 0
                end
                return true
            end
            suricatta.notify.error("Validation Error: JSON key '%s/%s' (%s) is missing.", p, k, type(v.type))
            return false
        end
        if type(def[k]) ~= type(v.type) then
            suricatta.notify.error(
                "Validation Error: JSON key '%s/%s' has wrong type (is %s != %s).",
                p,
                k,
                type(def[k]),
                type(v.type)
            )
            return false
        end
        if type(def[k]) == "string" then
            local slen = #def[k]
            if v.min and slen < v.min then
                suricatta.notify.error("Validation Error: JSON key '%s/%s' < %s", p, k, tostring(v.min))
                return false
            end
            if not v.min and slen == 0 then
                suricatta.notify.error("Validation Error: JSON key '%s/%s' is empty.", p, k)
                return false
            end
            if v.max and slen > v.max then
                suricatta.notify.error("Validation Error: JSON key '%s/%s' > %s", p, k, tostring(v.max))
                return false
            end
        end
        if type(def[k]) == "number" then
            if v.min and def[k] < v.min then
                suricatta.notify.error("Validation Error: JSON key '%s/%s' < %s", p, k, tostring(v.min))
                return false
            end
            if v.max and def[k] > v.max then
                suricatta.notify.error("Validation Error: JSON key '%s/%s' > %s", p, k, tostring(v.max))
                return false
            end
        end
        if type(v.type) == "table" then
            if not M.utils.validate_json_schema(def[k], v.value, ("%s/%s"):format(p, k)) then
                return false
            end
        end
        return true
    end
    path = path or ""
    if type(schema) ~= "table" then
        suricatta.notify.error("Validation Error: Schema at '%s' is not Table.", path)
        return false
    end
    for k, v in pairs(schema) do
        if not check_types(data, k, v, path) then
            return false
        end
    end
    if schema[1] then
        for i, _ in ipairs(data) do
            if not check_types(data, i, schema[1], path) then
                return false
            end
        end
    end
    return true
end

-- ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
-- ▏
-- ▏  wfx Job Definitions & Types
-- ▏
-- ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔

--- @class state
--
-- Enum definitions for states.
--
M.state = {
    --- @enum state.types
    --- Type of a state, used to classify state semantics.
    types = {
        --- A regular (i.e. none of the other) state of the workflow
        REGULAR = "REGULAR",
        --- A successful terminal state of the workflow
        CLOSED = "CLOSED",
        --- A failed terminal state of the workflow
        FAILED = "FAILED",
    },
}

--- @class transition
--
-- Enum definitions for transitions.
--
M.transition = {
    --- @enum transition.eligibles
    --- wfx's `EligibleEnum` enum values.
    eligibles = {
        --- CLIENT-eligible transition
        CLIENT = "CLIENT",
        --- WFX-eligible transition
        WFX = "WFX",
    },
    --- @enum transition.result
    --- Transition execution function result enum values.
    result = {
        --- Transition execution function completed successfully, yield to wfx
        COMPLETED = "COMPLETED",
        --- Transition execution function failed, try next
        FAIL_NEXT = "FAIL_NEXT",
        --- Transition execution function failed, yield to wfx
        FAIL_YIELD = "FAIL_YIELD",
    },
}

--- Functionality-augmented wfx `Job` instance.
--
--- @class job
--- @field id           string           Unique job ID
--- @field clientId     string           Device ID
--- @field definition   job.definition   DeviceArtifactUpdate definition
--- @field history      any              Job history (unused)
--- @field mtime        string           Date and time (ISO8601) when the job was last modified
--- @field stime        string           Date and time (ISO8601) when the job was created
--- @field status       job.status       Job Status Information
--- @field tags         string[]         Job tags (unused)
--- @field workflow     job.workflow     Job's Workflow
M.job = setmetatable({}, {
    --- @param  self     job  This job instance
    --- @param  wfx_job  job  wfx job
    --- @return job?          # This job instance with job data from wfx or `nil` on error
    __call = function(self, wfx_job)
        local function mt(tab, indextab)
            local t = getmetatable(tab) or {}
            t.__index = indextab
            t.__newindex = function(_, key, value)
                if not indextab[key] then
                    error(
                        ("Internal Error: Contaminating job Table with %s=%s"):format(
                            tostring(key or "<key>"),
                            tostring(value or "<value>")
                        )
                    )
                end
                rawset(indextab, key, value)
            end
            return setmetatable(tab, t)
        end

        self.definition.meta = self
        self.status.meta = self
        self.workflow.meta = self

        M.job.status.normalize(wfx_job.status)
        self = mt(self, wfx_job)
        self.definition = mt(self.definition, wfx_job.definition)
        self.status = mt(self.status, wfx_job.status)
        self.workflow = mt(self.workflow, wfx_job.workflow)
        if not self:validate() or not self:prepare() then
            return
        end
        return self
    end,
})

--- Validate the job.
--
--- @return boolean  # `true` if the job is valid, `false` otherwise
function M.job:validate()
    local function validate_id()
        if self.clientId ~= M.device.id then
            suricatta.notify.error(
                "Validation Error: Job's client ID (%s) doesn't match ours (%s).",
                self.clientId,
                M.device.id
            )
            return false
        end
        return true
    end
    return validate_id() and self.definition:validate() and self.workflow:validate()
end

--- Prepare the job.
--
--- @return boolean  # `true` if preparation succeeded, `false` otherwise
function M.job:prepare()
    return self.definition:prepare() and self.workflow:prepare()
end

--- Mark the job as invalid.
function M.job:invalidate()
    if self.id then
        self.id = "0xDEAD1D"
    end
end

--- Test whether the job is invalid.
--
--- @return boolean  # `true` if the job is invalid, `false` otherwise
function M.job:invalid()
    return not self.id or self.id == "0xDEAD1D"
end

-- ┌───────────────────────────────────────────────────────────────────────────
-- │  Job Status
-- └───────────────────────────────────────────────────────────────────────────

--- Job status information.
--
--- @class job.status
--- @field state           string    Name of the Workflow state
--- @field clientId        string    Client which sent the status update
--- @field progress        number    Current job progress percentage [0,100]
--- @field message         string    State update reason message
--- @field context         string[]  Additional context information.
--- @field definitionHash  string    `job.definition` hash
--- @field meta            job       This job status' `job`
M.job.status = {
    --- Job status message JSON Schema.
    --
    --- @class job.status.json_schema
    --
    -- JSON schema to comply to when sending status updates to wfx.
    --
    json_schema = {
        clientId = { type = "string", min = 1 },
        state = { type = "string", min = 1 },
        progress = { type = 100, max = 100, min = 0 },
        message = { type = "string", max = 1024, min = 0 },
        context = { type = "string", max = 2000, delimiter = ", " },
    },
}
setmetatable(M.job.status, {
    __tostring = function(self)
        local context = ([[{ "lines": [%s] }]]):format(
            table.concat(M.utils.string.enquote(self.context or {}), self.json_schema.context.delimiter)
        )
        if
            not M.utils.validate_json_schema({
                state = self.state,
                clientId = M.device.id,
                progress = self.progress,
                message = self.message,
                context = context,
            }, self.json_schema)
        then
            error("Internal Error: Status message is schema-invalid.")
        end
        local status = {
            ([["state": "%s"]]):format(self.state),
            ([["clientId": "%s"]]):format(M.device.id),
            ([["progress": %.0f]]):format(self.progress),
            ([["message": "%s"]]):format(self.message),
            ([["context": %s]]):format(context),
        }
        return M.utils.string.escape(([[{%s}]]):format(table.concat(status, ", ")))
    end,
})

--- Update job status JSON schema from its OpenAPI (Swagger) definition.
--
--- @param  swagger  table  Swagger definition
--- @return boolean         # `true` if swagger was parsed, `false` otherwise
function M.job.status:update_json_schema(swagger)
    if type(swagger) ~= "table" then
        return false
    end
    local spec = (((swagger or {}).definitions or {}).JobStatus or {}).properties or {}
    if M.utils.table.is_empty(spec) then
        return false
    end
    for k, v in pairs(spec) do
        if self.json_schema[k] then
            local max = v.maxLength or v.maximum
            self.json_schema[k].max = max and tonumber(max) or nil
        end
    end
    return true
end

--- Normalize job status information.
--
--- @param  status  job.status  Job status instance to normalize
--- @return job.status          # Normalized `job.status` instance
function M.job.status.normalize(status)
    status.state = status.state or "0xDEAD57A7E"
    status.clientId = M.device.id
    status.progress = status.progress or 0
    status.message = status.message or ""
    status.context = status.context or {}
    return status
end

--- Set job status information.
--
--- @param  status  job.status  Data to set
--- @return job.status          # `self` for chaining
function M.job.status:set(status)
    if not status.state then
        error("Internal Error: job.status:set() must be called with the .state field set.")
    end
    if self.definitionHash and status.definitionHash and self.definitionHash ~= status.definitionHash then
        suricatta.notify.info("Job definition changed, getting new version from server...")
        self.meta.definition:update()
    end
    return M.utils.table.merge(self.normalize(self), status)
end

--- @class retry_settings
--- @field retries      number | nil  Number of (re-)tries
--- @field retry_sleep  number | nil  Number of seconds between retries

--- Send job status information and update job with response.
--
-- *Note*: Sends status information to wfx and SWUpdate's progress interface.
--
--- @param  chan?   suricatta.open_channel  The channel to send the status update over
--- @param  retry?  retry_settings          Retry configuration
--- @return boolean                         # `true` if job status information sent, `false` on error
function M.job.status:send(chan, retry)
    chan = chan or M.channel.main
    -- Last resort defaults as in `include/channel.h`
    local retries = (retry or {}).retries or chan.options.retries or 5
    local retry_sleep = (retry or {}).retry_sleep or chan.options.retry_sleep or 5
    local msg = tostring(self)
    return M.utils.do_retry(retries, retry_sleep, function()
        local res, _, data = chan.put {
            url = ("%s/jobs/%s/status"):format(chan.options.url, self.meta.id),
            content_type = "application/json",
            method = suricatta.channel.method.PUT,
            format = suricatta.channel.content.JSON,
            headers_to_send = {
                -- Filter out unneeded reply's context to save some bandwidth.
                ["X-Response-Filter"] = "del(.context)",
            },
            request_body = msg,
        }
        if not res then
            suricatta.notify.error(
                "Cannot transition to state '%s': HTTP Error %d.",
                self.state,
                data.http_response_code
            )
            return false
        end
        suricatta.notify.debug(("[%3s%%%%] %s"):format(self.progress, self.message))
        suricatta.notify.progress(("[%3s%%] %s"):format(self.progress, self.message))
        if not M.utils.table.is_empty((data or {}).json_reply or {}) then
            self:set(data.json_reply)
        end
        return true
    end)
end

-- ┌───────────────────────────────────────────────────────────────────────────
-- │  Job Workflow
-- └───────────────────────────────────────────────────────────────────────────

--- Function prototype transition execution functions adhere to.
--- @alias dispatch_fn fun(job.workflow.transition, ...):transition.result, ...

--- Job Workflow.
--
-- The `states` Table is indexable by a state's name.
-- The `transitions` Table, indexed by a state name, holds all CLIENT-eligible transitions from that state.
--
--- @class job.workflow
--- @field name         string                                                             Unique Workflow name
--- @field states       job.workflow.state[]      | {[string]: job.workflow.state}         States of the Workflow
--- @field transitions  job.workflow.transition[] | {[string]: job.workflow.transition[]}  Transitions of the Workflow
--- @field groups       job.workflow.group[]      | {[string]: job.workflow.group}         State Groups of the Workflow
--- @field meta         job                                                                This job.workflows' job
M.job.workflow = {
    --- Workflow `Group` type definition.
    --
    --- @class job.workflow.group
    --- @field name         string    Group name
    --- @field states       string[]  Name of States in this Group

    --- Workflow `state` type definition.
    --
    --- @class job.workflow.state
    --- @field name         string        State name
    --- @field description  string        Description of the state
    --- @field group        string | nil  Group the state belongs to
    --- @field type         state.types   State's type ∈ { REGULAR, CLOSED, FAILED }

    --- Workflow `Transition` type definition.
    --
    --- @class job.workflow.transition
    --- @field from         job.workflow.state     Source state
    --- @field to           job.workflow.state     Target state
    --- @field description  string                 Description of the transition
    --- @field eligible     transition.eligibles   Actor that may execute the transition
    --- @field execute      dispatch_fn            Transition execution function
}

--- Transition execution function Dispatch.
--
-- Table holding transition execution functions and their access functions.
--
--- @class job.workflow.dispatch
M.job.workflow.dispatch = {}

--- Get a transition execution function.
--
--- @param  from  string  Source state name
--- @param  to    string  Target state name
--- @return dispatch_fn?  # Transition execution function
function M.job.workflow.dispatch:get(from, to)
    return (self.__funcs or {})[from .. "→" .. to]
end

--- Set a transition execution function.
--
--- @param from  string       Source state name
--- @param to    string       Target state name
--- @param fun   dispatch_fn  Transition execution function
function M.job.workflow.dispatch:set(from, to, fun)
    self.__funcs = self.__funcs or {}
    self.__funcs[from .. "→" .. to] = fun
end

--- Validate job Workflow.
--
--- @return boolean  # `true` if the job's workflow is valid, `false` otherwise
function M.job.workflow:validate()
    for _, trans in ipairs(self.transitions) do
        -- Note: :validate() is called prior to :prepare(), so not augmented yet.
        --- @cast trans { [string]: string }
        if trans.eligible == M.transition.eligibles.CLIENT and not self.dispatch:get(trans.from, trans.to) then
            suricatta.notify.error(
                "Validation Error: Transition execution function %s → %s not defined.",
                trans.from,
                trans.to
            )
            return false
        end
    end
    return true
end

--- Augment the Workflow with meta functionality.
--
-- *Note*: wfx makes sure a workflow is sound, i.e., if a state
-- is referenced in a transition, it's actually a valid state.
--
--- @param  self  job.workflow  This `job.workflow` instance
--- @return boolean  # `true` if preparation succeeded, `false` otherwise
function M.job.workflow:prepare()
    local group_type = {}
    for _, n in ipairs(M.dau.wfx_group.failed) do
        group_type[n] = M.state.types.FAILED
    end
    for _, n in ipairs(M.dau.wfx_group.closed) do
        group_type[n] = M.state.types.CLOSED
    end

    local state_group = {}
    for _, g in ipairs(self.groups or {}) do
        for _, s in ipairs(g.states) do
            state_group[s] = g.name
        end
        self.groups[g.name] = g
    end
    for _, s in ipairs(self.states or {}) do
        s.group = state_group[s.name]
        s.type = group_type[s.group] or M.state.types.REGULAR
        self.states[s.name] = s
    end

    for i, t in ipairs(self.transitions or {}) do
        local trans = {
            from = self.states[t.from],
            to = self.states[t.to],
            eligible = t.eligible,
            execute = self.dispatch:get(self.states[t.from].name, self.states[t.to].name),
        }
        self.transitions[i] = trans
        if trans.eligible == M.transition.eligibles.CLIENT then
            self.transitions[trans.from.name] = self.transitions[trans.from.name] or {}
            self.transitions[trans.from.name][#self.transitions[trans.from.name] + 1] = trans
        end
    end
    return true
end

--- Execute a transition.
--
--- @param  self  job.workflow  This `job.workflow` instance
--- @param  from  string        Source state name
--- @param  to    string        Target state name
--- @param  ...      any        Additional data passed to the transition execution function, if any
--- @return transition.result   # Result of transition execution function or `nil` if none found
--- @return ...?                # Additional return values of the transition execution function
function M.job.workflow:call(from, to, ...)
    --- @type job.workflow.transition
    local trans = M.utils.table.ifilter(self.transitions[from] or {}, function(t)
        --- @cast t job.workflow.transition
        return t.to.name ~= to
    end)[1]
    if not trans then
        error(("Runtime Error: More than one transition %s → %s or not existent!"):format(from, to))
    end
    suricatta.notify.info(
        "Invoking transition %s:%s → %s:%s",
        trans.from.type,
        trans.from.name,
        trans.to.type,
        trans.to.name
    )
    return trans:execute(...)
end

--- Helper function actually executing transition execution function(s).
--
--- @param  self     job.workflow                          This `job.workflow` instance
--- @param  silent   boolean                               Whether to be verbose or not
--- @param  func     fun(job.workflow.transition):boolean  Filter function; evaluates to `true` to filter out item
--- @param  ...      any                                   Additional data passed to the transition execution function, if any
--- @return transition.result                              # Result of transition execution function
--- @return ...?                                           # Additional return values of the transition execution function
local function do_advance(self, silent, func, ...)
    local message = {
        [M.transition.result.COMPLETED] = nil,
        [M.transition.result.FAIL_NEXT] = "Transition %s → %s execution failed, trying next.",
        [M.transition.result.FAIL_YIELD] = "Transition %s → %s execution failed, yielding to wfx.",
    }
    for trans in M.dau.sequencer(M.utils.table.ifilter(self.transitions[self.meta.status.state] or {}, func)) do
        if not silent then
            local msg = (select(1, ...))
            --- @cast trans job.workflow.transition
            suricatta.notify.info(
                "Executing transition %s:%s → %s:%s%s",
                trans.from.type,
                trans.from.name,
                trans.to.type,
                trans.to.name,
                type(msg) == "string" and (" (%s)"):format(msg) or ""
            )
        end
        local result = table.pack(trans:execute(...))
        if message[result] then
            suricatta.notify.debug(string.format(message[result[1]], trans.from.name, trans.to.name))
        end
        if M.utils.table.contains({ M.transition.result.COMPLETED, M.transition.result.FAIL_YIELD }, result[1]) then
            return table.unpack(result)
        end
    end
    suricatta.notify.debug("No (further) transition to execute found.")
    return M.transition.result.FAIL_YIELD
end

--- Advance Workflow executing client transitions.
--
--- @param  self  job.workflow  This `job.workflow` instance
--- @param  ...   any           Additional data passed to the transition execution function, if any
--- @return transition.result   # Result of transition execution function
--- @return ...?                # Additional return values of the transition execution function
function M.job.workflow:advance(...)
    return do_advance(self, false, function(t)
        --- @cast t job.workflow.transition
        return not M.utils.table.contains({ M.state.types.CLOSED, M.state.types.REGULAR }, t.to.type)
    end, ...)
end

--- Test if Workflow can be advanced by the client from its current state.
--
--- @param  self  job.workflow  This `job.workflow` instance
--- @return boolean  # `true` if Workflow can be advanced from its current state, `false` otherwise
function M.job.workflow:can_advance()
    return not M.utils.table.is_empty(
            M.utils.table.ifilter(self.transitions[self.meta.status.state] or {}, function(t)
                --- @cast t job.workflow.transition
                return not M.utils.table.contains({ M.state.types.CLOSED, M.state.types.REGULAR }, t.to.type)
            end)
        )
end

--- Terminate Workflow processing with Failure.
--
--- @param  self  job.workflow  This `job.workflow` instance
--- @param  ...   any           Additional data passed to the transition execution function, if any
--- @return transition.result   # Result of transition execution function
--- @return ...?                # Additional return values of the transition execution function
function M.job.workflow:fail(...)
    return do_advance(self, false, function(t)
        --- @cast t job.workflow.transition
        return t.to.type ~= M.state.types.FAILED
    end, ...)
end

--- Report in-state progress to wfx.
--
--- @param  self  job.workflow  This `job.workflow` instance
--- @param  ...   any           Additional data passed to the transition execution function, if any
--- @return transition.result   # Result of transition execution function
--- @return ...?                # Additional return values of the transition execution function
function M.job.workflow:report_progress(...)
    return do_advance(self, true, function(t)
        --- @cast t job.workflow.transition
        return t.to.type ~= M.state.types.REGULAR or t.from.name ~= t.to.name
    end, ...)
end

--- Test if the current state is a terminal state.
--
--- @param  self    job.workflow  This `job.workflow` instance
--- @param  status  job.status    Job status to test for terminal state
--- @return boolean?              # `true` if current state is terminal, `false` otherwise
function M.job.workflow:terminated(status)
    M.job.status.normalize(status)
    return M.utils.table.contains(
        { M.state.types.FAILED, M.state.types.CLOSED },
        (self.states[status.state] or {}).type
    )
end

-- ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
-- ▏
-- ▏  DeviceArtifactUpdate
-- ▏
-- ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔

--- DeviceArtifactUpdate ― Semantic Workflow Group Mapping.
--
--- @class dau.wfx_group
--- @field query     string[]  Group name(s) used when querying wfx for (new) jobs
--- @field failed    string[]  Group name(s) of a failed update's terminal states
--- @field closed    string[]  Group name(s) of a successful update's terminal states
M.dau.wfx_group = {
    query = { "OPEN" },
    failed = { "FAILED" },
    closed = { "CLOSED" },
}

--- @enum state.dau
--- State names in the DeviceArtifactUpdate Workflow Family
M.state.dau = {
    DOWNLOAD = "DOWNLOAD",
    DOWNLOADING = "DOWNLOADING",
    DOWNLOADED = "DOWNLOADED",
    INSTALL = "INSTALL",
    INSTALLING = "INSTALLING",
    INSTALLED = "INSTALLED",
    ACTIVATE = "ACTIVATE",
    ACTIVATING = "ACTIVATING",
    ACTIVATED = "ACTIVATED",
    TERMINATED = "TERMINATED",
}

--- Transition Sequencer Iterator.
--
-- The transition sequencer iterator returns the transition to be executed next
-- out of a set of eligible transitions, in a Workflow (Family)-specific order.
--
-- This simple algorithm sorts the given transitions into 3 buckets: REGULAR,
-- CLOSED, and FAILED. The REGULAR transition bucket is sorted so that self-
-- looping transitions are prioritized, i.e., "non-greedy". Ordering the CLOSED
-- and FAILED transition bucket is redundant as each results in a terminal state.
-- This sorted list is returned as an iterator.
--
--- @param  transitions  job.workflow.transition[]  Eligible transitions
--- @return function                                # The iterator function
--- @return table                                   # The modified `transitions` table
function M.dau.sequencer(transitions)
    local trans = {
        [M.state.types.REGULAR] = 1,
        [M.state.types.CLOSED] = 2,
        [M.state.types.FAILED] = 3,
        [1] = {},
        [2] = {},
        [3] = {},
    }
    for _, t in ipairs(transitions or {}) do
        local bucket = trans[t.to.type]
        trans[bucket][#trans[bucket] + 1] = t
    end
    table.sort(trans[1], function(a, _)
        --- @cast a job.workflow.transition
        if a.from.name == a.to.name then
            -- Sort self-looping transitions first.
            return true
        end
        return false
    end)
    local i = { bucket = 1, index = 0 }
    local function get(tab)
        i.index = i.index + 1
        if tab[i.bucket][i.index] ~= nil then
            return tab[i.bucket][i.index]
        end
        i.bucket, i.index = i.bucket + 1, 0
        if not tab[i.bucket] then
            return
        end
        return get(tab)
    end
    return get, trans
end

-- ┌───────────────────────────────────────────────────────────────────────────
-- │  DeviceArtifactUpdate :: Definition
-- └───────────────────────────────────────────────────────────────────────────

--- Default update artifact type (i.e., activation mode).
M.dau.DEFAULT_UPDATE_TYPE = "firmware"

--- DeviceArtifactUpdate ― Job Definition.
--
--- @class job.definition
--- @field version    string                         Version of the Update
--- @field type       string[] | {[string]: number}  Type of the Update
--- @field artifacts  job.definition.artifact[]      List of Artifacts to install
--- @field meta       job                            This job definitions' `job`
M.job.definition = {}

--- DeviceArtifactUpdate ― Artifacts Definition.
--
--- @class job.definition.artifact
--- @field name        string  (File) name of the artifact
--- @field version     string  Version of the Artifact
--- @field uri         string  URI where the artifact can be downloaded

--- DeviceArtifactUpdate ― Definition JSON Schema
--- @class job.definition.json_schema
M.job.definition.json_schema = {
    version = { type = "string" },
    type = { type = {}, value = { [1] = { type = "string" } } },
    artifacts = {
        type = {},
        value = {
            [1] = {
                type = {},
                value = {
                    name = { type = "string", optional = true },
                    version = { type = "string", optional = true },
                    uri = { type = "string" },
                },
            },
        },
    },
}

--- Update job Definition from server.
--
-- *Note*: This is a synchronous operation since the next transition execution
-- function may rely on a changed job definition's data.
--
--- @param chan?  suricatta.open_channel  The channel to get the definition update over
function M.job.definition:update(chan)
    chan = chan or M.channel.main
    M.utils.do_retry(math.huge, chan.options.retry_sleep, function()
        local res, _, data = chan.get {
            url = ("%s/job/%s/definition"):format(chan.options.url, self.meta.id),
        }
        if not res then
            suricatta.notify.warn("Got HTTP error code %d from server.", data.http_response_code)
            return false
        end
        if M.utils.table.is_empty((data or {}).json_reply or {}) then
            suricatta.notify.warn("Got bad JSON response from server: %q", tostring(data))
            return false
        end
        if not self:validate(data.json_reply) then
            return false
        end
        local t = getmetatable(self)
        t.__index = data.json_reply
        self = setmetatable(self, t)
        return true
    end)
end

--- Prepare and Check Job definition.
--
--- @return boolean  # `true` if preparation succeeded, `false` otherwise
function M.job.definition:prepare()
    if #table.concat(self.type, ":") > 2048 then
        suricatta.notify.error("Validation Error: definition's .type is > %d chars.", 2048 - (#self.type - 1))
        return false
    end
    for i, v in ipairs(self.type) do
        self.type[v] = i
    end
    return true
end

--- Validate job Definition.
--
--- @param  definition  job.definition?  Job definition other than `self`'s
--- @return boolean                      # `true` if the job's definition is valid, `false` otherwise
function M.job.definition:validate(definition)
    return M.utils.validate_json_schema(definition or self, self.json_schema) and self:prepare()
end

--- Test whether update artifact is of (default) type "firmware".
--
-- If one of `.type`'s tags matches `DEFAULT_UPDATE_TYPE` == "firmware", only then the
-- update is to be firmware-activated, i.e., via rebooting into the new firmware.
--
-- Other tags are passed verbatim to progress client(s), concatenated with ":",
-- for them to take according activation actions.
--
--- @return boolean  # `true` if activation method is "firmware" (default), `false` if not
function M.job.definition:is_firmware()
    return self.type[M.dau.DEFAULT_UPDATE_TYPE] ~= nil
end

-- ┌───────────────────────────────────────────────────────────────────────────
-- │  DeviceArtifactUpdate :: Transitions
-- └───────────────────────────────────────────────────────────────────────────

--- Helper function to truncate installation logs to maximal length.
--
--- @param  log        table   Table with log lines
--- @param  maxlength  number  Maximal String length
--- @param  delimiter  string  JSON list delimiter
--- @return string[]           # Table with log lines, `delimiter`-concatenated less than `maxlength` characters
local function prepare_logs(log, maxlength, delimiter)
    local details = {}
    -- Some headroom for JSON enclosure in status message.
    local length = 50
    for index, line in M.utils.ripairs(log or {}) do
        local message = ([[%d: %s]]):format(index, M.utils.string.escape(line))
        -- Add JSON list delimiter length plus \n to string length
        length = length + #message + #delimiter + 1
        if length > maxlength then
            break
        end
        table.insert(details, 1, message)
    end
    return details
end

--- @class job_status
--- @field [1]        job.workflow.transition  This `job.workflow.transition` instance
--- @field [2]        job                      This `job` instance
--- @field state?     string                   Target state, defaults to this transition's `to.name`
--- @field message?   string                   State update reason message
--- @field progress?  number                   Current job progress percentage [0,100]
--- @field context?   string[]                 Additional context information.
--- @field channel?   suricatta.open_channel   The channel to send the status update over
--- @field retry?     retry_settings           Retry settings, else channel defaults are used

--- Helper function to send & sync Job Status with wfx.
--
-- *Note*: `job.status.state` is set to this transition's `to` state.
--
--- @param  status  job_status
--- @return boolean
local function sync_job_status(status)
    if #status > 2 then
        error("Internal Error: sync_job_status() called with > 2 positional parameters.")
    end
    local transition = status[1]
    local job = status[2]
    return job.status
        ---@diagnostic disable-next-line: missing-fields
        :set({
            state = status.state or transition.to.name,
            message = status.message or "",
            progress = status.progress or 100,
            context = status.context or {},
        })
        :send(status.channel, status.retry)
end

--- Helper function to send update activation progress message.
--
-- Send update activation message via SWUpdate's progress interface
-- to subscribed progress clients.
--
--- @param job  job
local function send_progress_activation_msg(job)
    suricatta.notify.progress(M.utils.string
        .escape([[
        {
            "source": %s,
            "module": "wfx",
            "state": "ACTIVATING",
            "progress": 100,
            "message": "%s"
        }
        ]])
        :format(suricatta.ipc.sourcetype.SOURCE_SURICATTA, table.concat(job.definition.type, ":")))
end

--- Helper function to set persistent bootloader state.
--
-- Storing the persistent state `pstate` is retried up to 3 times
-- with 3 seconds pause in between each try if it has failed.
--
--- @param  pstate  number  Any of `suricatta.pstate`'s enum values
--- @return boolean         # `true` if persistent state is set, `false` otherwise
local function save_pstate(pstate)
    suricatta.notify.debug("Setting persistent state to %s.", suricatta.pstate[pstate])
    return M.utils.do_retry(3, 3, function()
        if suricatta.pstate.save(pstate) then
            suricatta.notify.debug("Persistent state set to %s.", suricatta.pstate[pstate])
            return true
        end
        suricatta.notify.error("Error setting persistent state information, retrying in a few seconds.")
        return false
    end)
end

--- Helper function to check if `pstate` allows starting a new job.
--
--- @param  self  job.workflow.transition
--- @param  job   job
--- @return boolean
local function is_pstate_ok(self, job)
    if M.device.pstate == suricatta.pstate.INSTALLED then
        suricatta.notify.warn("Started new job while last one's activation is pending, notifying progress.")
        send_progress_activation_msg(job)
        return false
    end
    if M.utils.table.contains({ suricatta.pstate.FAILED, suricatta.pstate.TESTING }, M.device.pstate) then
        local msg = ("Persistent state is %s at job start, spurious update test?"):format(
            suricatta.pstate[M.device.pstate] or "N/A"
        )
        suricatta.notify.warn(msg)
        sync_job_status { self, job, message = msg, progress = 0 }
    end
    return true
end

M.job.workflow.dispatch:set(
    M.state.dau.DOWNLOAD,
    M.state.dau.DOWNLOADING,
    --- @param  self  job.workflow.transition
    --- @param  job   job
    --- @return transition.result
    function(self, job)
        local ok = is_pstate_ok(self, job)
        if not ok then
            return M.transition.result.FAIL_YIELD
        end

        if not sync_job_status { self, job, message = ("Start %s."):format(self.from.name), progress = 0 } then
            return M.transition.result.FAIL_YIELD
        end

        M.channel.progress = M.channel(M.utils.table.merge({ noprogress = true }, M.channel.main.options))
        if not M.channel.progress then
            suricatta.notify.warn("Cannot initialize progress reporting channel, won't send progress.")
        end

        suricatta.notify.debug(
            "%s Version '%s' (Type: %s).",
            self.to.name,
            job.definition.version,
            table.concat(job.definition.type, ":")
        )

        for count, artifact in pairs(job.definition.artifacts) do
            local source_uri = M.utils.string.escape(artifact.uri, { ['"'] = '"', ["\\"] = "" })
            local target_uri = ("%s%s_%d.swu"):format(suricatta.get_tmpdir(), job.id, count)
            suricatta.notify.debug(
                "[  0%%] [%s] Artifact %d/%d: '%s' from %q.",
                self.to.name,
                count,
                #job.definition.artifacts,
                artifact.name,
                source_uri
            )
            local res, _, updatelog = suricatta.download({ channel = M.channel.main, url = source_uri }, target_uri)
            if not res then
                local msg = ("Error downloading artifact %d/%d."):format(count, #job.definition.artifacts)
                suricatta.notify.error(msg)
                return job.workflow:fail(
                    job,
                    msg,
                    prepare_logs(
                        updatelog,
                        job.status.json_schema.context.max,
                        job.status.json_schema.context.delimiter
                    )
                )
            end
            sync_job_status {
                self,
                job,
                message = ("Artifact %d/%d download finished."):format(count, #job.definition.artifacts),
                progress = math.floor(100 / #job.definition.artifacts * count),
            }
        end

        if M.channel.progress then
            M.channel.progress.close()
            M.channel.progress = nil
        end

        sync_job_status { self, job, message = "Finished." }

        -- Tail-call transition DOWNLOADING → DOWNLOADED without detour via wfx query.
        return job.workflow:call(M.state.dau.DOWNLOADING, M.state.dau.DOWNLOADED, job)
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.DOWNLOAD,
    M.state.dau.TERMINATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @return transition.result
    function(self, job)
        if not sync_job_status { self, job } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.DOWNLOADING,
    M.state.dau.DOWNLOADING,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @param  message?  suricatta.ipc.progress_msg
    --- @return transition.result
    function(self, job, message)
        if not message then
            -- Called as "regular" transition in response to wfx query.
            -- This happens if:
            -- * DOWNLOADING → TERMINATED has failed or
            -- * DOWNLOADING → DOWNLOADED has failed.
            -- In both cases, terminate the update.
            return job.workflow:fail(job, "wfx sync has failed, terminating update.")
        end
        -- Called as progress reporting callback transition.
        if not M.channel.progress or not message.source == suricatta.ipc.sourcetype.SOURCE_SURICATTA then
            return M.transition.result.FAIL_YIELD
        end
        if message.status == suricatta.ipc.RECOVERY_STATUS.DOWNLOAD then
            if M.server.progress_rate_limited(message.dwl_percent, job.status.progress) then
                return M.transition.result.COMPLETED
            end
            -- Informational progress reports to wfx are not retried.
            sync_job_status {
                self,
                job,
                channel = M.channel.progress,
                message = "Downloading artifact",
                progress = message.dwl_percent,
                retry = { retries = 1 },
            }
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.DOWNLOADING,
    M.state.dau.DOWNLOADED,
    --- @param  self  job.workflow.transition
    --- @param  job   job
    --- @return transition.result
    function(self, job)
        if not sync_job_status { self, job, message = "Finished." } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.DOWNLOADING,
    M.state.dau.TERMINATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @param  message?  string
    --- @param  context?  string[]
    --- @return transition.result
    function(self, job, message, context)
        if not sync_job_status { self, job, message = message, context = context } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.INSTALL,
    M.state.dau.INSTALLING,
    --- @param  self  job.workflow.transition
    --- @param  job   job
    --- @return transition.result
    function(self, job)
        local ok = is_pstate_ok(self, job)
        if not ok then
            return M.transition.result.FAIL_YIELD
        end

        if job.workflow.transitions[M.state.dau.DOWNLOAD] then
            local function file_exists(name)
                local f = io.open(name, "r")
                return f ~= nil and io.close(f)
            end
            -- Check that all to be installed artifact files are downloaded.
            for count, _ in pairs(job.definition.artifacts) do
                local file = ("%s%s_%d.swu"):format(suricatta.get_tmpdir(), job.id, count)
                if not file_exists(file) then
                    local msg = ("Artifact %d/%d not downloaded to '%s'.."):format(
                        count,
                        #job.definition.artifacts,
                        file
                    )
                    suricatta.notify.error(msg)
                    return job.workflow:fail(job, msg)
                end
            end
        end

        if not sync_job_status { self, job, message = ("Start %s."):format(self.from.name), progress = 0 } then
            return M.transition.result.FAIL_YIELD
        end

        M.channel.progress = M.channel(M.utils.table.merge({ noprogress = true }, M.channel.main.options))
        if not M.channel.progress then
            suricatta.notify.warn("Cannot initialize progress reporting channel, won't send progress.")
        end

        suricatta.notify.progress(M.utils.string.escape([[{"%s": { "reboot-mode" : "no-reboot"}}]])
            :format(suricatta.ipc.progress_cause.CAUSE_REBOOT_MODE))

        suricatta.notify.debug(
            "%s Version '%s' (Type: %s).",
            self.to.name,
            job.definition.version,
            table.concat(job.definition.type, ":")
        )

        for count, artifact in pairs(job.definition.artifacts) do
            local source_uri = M.utils.string.escape(artifact.uri, { ['"'] = '"', ["\\"] = "" })
            if job.workflow.transitions[M.state.dau.DOWNLOAD] then
                source_uri = ("file://%s%s_%d.swu"):format(suricatta.get_tmpdir(), job.id, count)
            end
            suricatta.notify.debug(
                "[  0%%] Artifact %d/%d: '%s' from %q",
                count,
                #job.definition.artifacts,
                artifact.name,
                source_uri
            )
            local res, _, updatelog = suricatta.install { channel = M.channel.main, url = source_uri }
            -- Unconditionally remove the artifact file ignoring errors, e.g., if run in streaming mode.
            os.remove(("%s%s_%d.swu"):format(suricatta.get_tmpdir(), job.id, count))
            if not res then
                local msg = ("Error installing artifact %d/%d."):format(count, #job.definition.artifacts)
                suricatta.notify.error(msg)
                return job.workflow:fail(
                    job,
                    msg,
                    prepare_logs(
                        updatelog,
                        job.status.json_schema.context.max,
                        job.status.json_schema.context.delimiter
                    )
                )
            end
            sync_job_status {
                self,
                job,
                message = ("Artifact %d/%d installation finished."):format(count, #job.definition.artifacts),
                progress = math.floor(100 / #job.definition.artifacts * count),
            }
        end

        if M.channel.progress then
            M.channel.progress.close()
            M.channel.progress = nil
        end

        sync_job_status { self, job, message = "Finished." }

        -- Tail-call transition INSTALLING → INSTALLED without detour via wfx query.
        return job.workflow:call(M.state.dau.INSTALLING, M.state.dau.INSTALLED, job)
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.INSTALL,
    M.state.dau.TERMINATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @return transition.result
    function(self, job)
        if not sync_job_status { self, job } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.INSTALLING,
    M.state.dau.INSTALLING,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @param  message?  suricatta.ipc.progress_msg
    --- @return transition.result
    function(self, job, message)
        if not message then
            -- Called as "regular" transition in response to wfx query.
            -- This happens if:
            -- * INSTALLING → TERMINATED has failed or
            -- * INSTALLING → INSTALLED has failed.
            -- In both cases, terminate the update.
            return job.workflow:fail(job, "wfx sync has failed, terminating update.")
        end

        if not M.channel.progress or not message.source == suricatta.ipc.sourcetype.SOURCE_SURICATTA then
            return M.transition.result.FAIL_YIELD
        end

        if message.status == suricatta.ipc.RECOVERY_STATUS.DOWNLOAD then
            if M.server.progress_rate_limited(message.dwl_percent, job.status.progress) then
                return M.transition.result.COMPLETED
            end
            local msg = "Downloading artifact"
            if job.workflow.transitions[M.state.dau.DOWNLOAD] then
                msg = ("Copying artifact to TMPDIR=%s"):format(suricatta.get_tmpdir())
            end
            -- Informational progress reports to wfx are not retried.
            sync_job_status {
                self,
                job,
                channel = M.channel.progress,
                message = msg,
                progress = message.dwl_percent,
                retry = { retries = 1 },
            }
            return M.transition.result.COMPLETED
        end

        if message.status == suricatta.ipc.RECOVERY_STATUS.PROGRESS then
            -- Don't report installing message if the 'dummy' handler is used or no current image is set.
            if #message.hnd_name == 0 or #message.cur_image == 0 or message.cur_step == 0 then
                return M.transition.result.COMPLETED
            end
            if M.server.progress_rate_limited(message.cur_percent, job.status.progress) then
                return M.transition.result.COMPLETED
            end
            -- Informational progress reports to wfx are not retried.
            sync_job_status {
                self,
                job,
                channel = M.channel.progress,
                message = ("Installing artifact %d/%d: '%s' with '%s' ..."):format(
                    message.cur_step,
                    message.nsteps,
                    message.cur_image,
                    message.hnd_name
                ),
                progress = message.cur_percent,
                retry = { retries = 1 },
            }
            return M.transition.result.COMPLETED
        end

        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.INSTALLING,
    M.state.dau.INSTALLED,
    --- @param  self  job.workflow.transition
    --- @param  job   job
    --- @return transition.result
    function(self, job)
        if not sync_job_status { self, job, message = "Finished." } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.INSTALLING,
    M.state.dau.TERMINATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @param  message?  string
    --- @param  context?  string[]
    --- @return transition.result
    function(self, job, message, context)
        if not sync_job_status { self, job, message = message, context = context } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.ACTIVATE,
    M.state.dau.ACTIVATING,
    --- @param  self  job.workflow.transition
    --- @param  job   job
    --- @return transition.result
    function(self, job)
        if not job.definition:is_firmware() then
            if M.device.pstate ~= suricatta.pstate.OK then
                suricatta.notify.warn("Persistent state is not OK (is %s).", suricatta.pstate[M.device.pstate])
                if not save_pstate(suricatta.pstate.OK) then
                    return M.transition.result.FAIL_YIELD
                end
                M.device.pstate = suricatta.pstate.OK
            end
            if not sync_job_status { self, job, message = "Notifying Progress Client(s) to activate artifact." } then
                return M.transition.result.FAIL_YIELD
            end
            send_progress_activation_msg(job)
            -- Tail-call transition ACTIVATING → ACTIVATED without detour via wfx query.
            return job.workflow:call(M.state.dau.ACTIVATING, M.state.dau.ACTIVATED, job)
        end

        suricatta.notify.debug("Job activation mode is 'firmware'.")
        if suricatta.pstate.get() == suricatta.pstate.INSTALLED then
            suricatta.notify.warn("Persistent state is already set to INSTALLED.")
        else
            if not save_pstate(suricatta.pstate.IN_PROGRESS) or not save_pstate(suricatta.pstate.INSTALLED) then
                return M.transition.result.FAIL_YIELD
            end
        end
        M.device.pstate = suricatta.pstate.INSTALLED

        if not sync_job_status { self, job, message = "Set persistent state marker to INSTALLED." } then
            return M.transition.result.FAIL_YIELD
        end
        -- Tail-call transition ACTIVATING → ACTIVATING without detour via wfx query.
        return job.workflow:call(M.state.dau.ACTIVATING, M.state.dau.ACTIVATING, job, M.device.pstate)
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.ACTIVATE,
    M.state.dau.TERMINATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @return transition.result
    function(self, job)
        if not sync_job_status { self, job } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.ACTIVATING,
    M.state.dau.ACTIVATING,
    --- @param  self     job.workflow.transition
    --- @param  job      job
    --- @param  pstate?  number
    --- @return transition.result
    function(self, job, pstate)
        if not job.definition:is_firmware() then
            -- Called as "regular" transition in response to wfx query.
            -- This happens if the tail-call ACTIVATING → ACTIVATED has
            -- failed. As progress clients have been notified, mark this
            -- update as ACTIVATED.
            return job.workflow:call(M.state.dau.ACTIVATING, M.state.dau.ACTIVATED, job)
        end

        -- Refresh `pstate` if dispatched in response to wfx query.
        M.device.pstate = pstate or suricatta.pstate.get()

        if M.device.pstate == suricatta.pstate.OK then
            local msg = "Persistent state is already set to OK. Finished update."
            suricatta.notify.info(msg)
            return job.workflow:call(M.state.dau.ACTIVATING, M.state.dau.ACTIVATED, job, msg)
        end

        if M.device.pstate == suricatta.pstate.INSTALLED then
            sync_job_status { self, job, message = "Update installed. Notifying Progress Client(s) to reboot." }
            send_progress_activation_msg(job)
            return M.transition.result.COMPLETED
        end

        if M.device.pstate == suricatta.pstate.TESTING then
            local msg = ("Update activation SUCCESSFUL, finalizing job %s."):format(job.id)
            suricatta.notify.info(msg)
            if not save_pstate(suricatta.pstate.OK) then
                return M.transition.result.FAIL_YIELD
            end
            M.device.pstate = suricatta.pstate.OK
            return job.workflow:call(M.state.dau.ACTIVATING, M.state.dau.ACTIVATED, job, msg)
        end

        if M.device.pstate == suricatta.pstate.FAILED then
            local msg = ("Update activation FAILED, failing job %s ..."):format(job.id)
            suricatta.notify.error(msg)
            if not save_pstate(suricatta.pstate.OK) then
                return M.transition.result.FAIL_YIELD
            end
            M.device.pstate = suricatta.pstate.OK
            return job.workflow:fail(job, msg)
        end

        local msg = ("%s → %s called with unhandled persistent state %s."):format(
            self.from.name,
            self.to.name,
            suricatta.pstate[M.device.pstate] or "N/A"
        )
        suricatta.notify.error(msg)
        sync_job_status { self, job, msg }
        return M.transition.result.FAIL_YIELD
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.ACTIVATING,
    M.state.dau.ACTIVATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @param  message?  string
    --- @return transition.result
    function(self, job, message)
        if not sync_job_status { self, job, message = message or "Finished Update." } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

M.job.workflow.dispatch:set(
    M.state.dau.ACTIVATING,
    M.state.dau.TERMINATED,
    --- @param  self      job.workflow.transition
    --- @param  job       job
    --- @param  message?  string
    --- @return transition.result
    function(self, job, message)
        if not sync_job_status { self, job, message = message } then
            return M.transition.result.FAIL_YIELD
        end
        return M.transition.result.COMPLETED
    end
)

-- ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
-- ▏
-- ▏  Suricatta wfx-Binding Types & Definitions
-- ▏
-- ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔

--- Device state and information.
--
--- @class device
--- @field pstate  number            Persistent state ID number ∈ suricatta.pstate
--- @field id      string            Device ID
--- @field version string            Device client version information, sent to server in HTTP Header
--- @field reset   function(device)  Reset `device` Table
M.device = {
    version = ("SWUpdate %s.%s"):format(table.unpack(suricatta.getversion() or { 2023, 5 })),
    reset = function(self)
        self.id = nil
        self.pstate = nil
    end,
}

--- Device ↔ Server communication channel state and information.
--
--- @type {[string]: suricatta.open_channel}
M.channel = setmetatable({}, {
    __call = function(_, opts)
        local res, chan = suricatta.channel.open(opts)
        if not res then
            return nil
        end
        return chan
    end,
})

--- Server information.
--
--- @class server
--- @field polldelay  number            Delay between two wfx poll operations in seconds
--- @field reset      function(server)  Reset `server` Table
M.server = {
    reset = function(self)
        self.polldelay = nil
    end,
}

--- Rate-limit the number of progress messages to wfx.
--
--- @param  percent  number  Progress in percent
--- @param  last     number  Last Progress in percent
--- @return boolean          # `true` if progress message shouldn't be sent
M.server.progress_rate_limited = function(percent, last)
    local scale = 10 / 100
    return math.floor(scale * last) == math.floor(scale * percent)
end

-- ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
-- ▏
-- ▏  Suricatta Interface Implementation
-- ▏
-- ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔

--- Progress thread callback handler reporting progress to wfx.
--
--- @param  message  suricatta.ipc.progress_msg  The progress message
--- @return suricatta.status                     # Suricatta return code
function M.suricatta_funcs.progress_callback(message)
    if M.job.workflow:report_progress(M.job, message) == M.transition.result.COMPLETED then
        return suricatta.status.OK
    end
    return suricatta.status.EERR
end
suricatta.server.register(M.suricatta_funcs.progress_callback, suricatta.server.CALLBACK_PROGRESS)

--- Query and handle pending actions on the wfx.
--
-- Lua counterpart of `server_op_res_t server_has_pending_action(int *action_id)`.
--
-- The suricatta.status return values `UPDATE_AVAILABLE` and `ID_REQUESTED` are handled
-- in `suricatta/suricatta.c`, others result in SWUpdate sleeping.
--
--- @return suricatta.status  # Suricatta return code
function M.suricatta_funcs.has_pending_action()
    --- Perform a GET request on the wfx.
    --
    --- @param  url  string  URL to query
    --- @return boolean      # `true` if wfx responded, `false` on network error
    --- @return table?       # Lua Table-ified JSON response or `nil` on error
    local function query_wfx(url)
        suricatta.notify.debug("Suricatta querying %q", url)
        local res, _, data = M.channel.main.get {
            url = url,
        }
        if not res then
            suricatta.notify.debug("Got HTTP error code %d from server.", data.http_response_code)
            return false
        end
        if M.utils.table.is_empty((data or {}).json_reply or {}) then
            suricatta.notify.warn("Got bad JSON response from server: %q", tostring(data))
            return true
        end
        return true, data.json_reply
    end

    --- Query the wfx for a new job to process.
    --
    --- @return boolean  # `true` if wfx responded, `false` on network error
    --- @return job?     # New job to process or `nil` on error
    local function get_new_job()
        local response, data = query_wfx(
            ("%s/jobs?clientId=%s&group=%s"):format(
                M.channel.main.options.url,
                M.device.id,
                table.concat(M.dau.wfx_group.query, ",")
            )
        )
        for _, j in ipairs((data or {}).content or {}) do
            --- @cast j job
            if M.job(j) then
                return response, j
            end
            suricatta.notify.error("Job validation failed, skipping job.")
        end
        return response
    end

    --- Get the current job, if any, to process further from the wfx.
    --
    --- @return boolean  # `true` if wfx responded, `false` on network error
    --- @return job?     # Current job as wfx sees it or `nil` on error
    local function get_current_job()
        if M.job:invalid() then
            return true
        end
        return query_wfx(("%s/jobs/%s"):format(M.channel.main.options.url, M.job.id))
    end

    -- ────┤ Handle & Dispatch ├──────────────────────────────────────
    local response, j = get_current_job()
    if not response then
        suricatta.notify.debug("Got no response from server: sleeping ...")
        return suricatta.status.NO_UPDATE_AVAILABLE
    end
    if not j or M.job.workflow:terminated(j.status) then
        M.job:invalidate()
        response, j = get_new_job()
        if not response or not j then
            suricatta.notify.debug("Got no response or no job: sleeping ...")
            return suricatta.status.NO_UPDATE_AVAILABLE
        end
    end

    M.job.status:set(j.status)
    if not M.job.workflow:can_advance() then
        suricatta.notify.debug("No (further) transitions to take: sleeping ...")
        return suricatta.status.EAGAIN
    end

    suricatta.notify.info("Processing job ID %s in state %s ...", M.job.id, M.job.status.state)
    _ = M.job.workflow:advance(M.job)
    suricatta.notify.info("Yielding to wfx.")
    return suricatta.status.OK
end
suricatta.server.register(M.suricatta_funcs.has_pending_action, suricatta.server.HAS_PENDING_ACTION)

-- Lua counterpart of `server_op_res_t server_install_update(void)`.
--
-- This function is intended to be called by `suricatta/suricatta.c`'s event loop
-- via `has_pending_action()` returning `UPDATE_AVAILABLE`.
-- Here, this functionality is called directly from within `job.workflow:advance()`
-- so that a dummy implementation satisfies the requirement of this function
-- being registered.
suricatta.server.register(function()
    return suricatta.status.OK
end, suricatta.server.INSTALL_UPDATE)

--- Print the help text.
--
-- Lua counterpart of `void server_print_help(void)`.
--
--- @param  defaults  suricatta.channel.options|table  Default options
--- @return suricatta.status                           # Suricatta return code
function M.suricatta_funcs.print_help(defaults)
    defaults = defaults or {}
    local stdout = io.output()
    stdout:write("\t  -u, --url <URL>          * Base URL to the wfx server, e.g.,http://localhost:8080/api/wfx/v1\n")
    stdout:write("\t  -i, --id <string>        * The device ID\n")
    stdout:write("\t  -y, --proxy <URL>          Use proxy. Either proxy <URL> or ${http,all}_proxy is tried\n")
    stdout:write(
        ("\t  -p, --polldelay <int>      Delay between two poll operations (default: %s sec)\n"):format(
            tostring(defaults.polldelay or "?")
        )
    )
    stdout:write(
        ("\t  -r, --retry <int>          Retry count prior to failing (default: %s)\n"):format(
            tostring(defaults.retries or "?")
        )
    )
    stdout:write(
        ("\t  -w, --retrywait <int>      Delay between retries, e.g., download resume (default: %s sec)\n"):format(
            tostring(defaults.retry_sleep or "?")
        )
    )
    stdout:write("\t  -c, --confirm <ok|failed>  Confirm update test result\n")
    stdout:write("\t  -x, --nocheckcert          Ignore invalid server certificates\n")
    stdout:write("\t  -v                         Enable verbose log messages\n")
    return suricatta.status.OK
end
suricatta.server.register(M.suricatta_funcs.print_help, suricatta.server.PRINT_HELP)

--- Start the Suricatta server.
--
-- Lua counterpart of `server_op_res_t server_start(char *fname, int argc, char *argv[])`.
--
--- @param  defaults  {[string]: any}  Lua suricatta module channel default options
--- @param  argv      string[]         C's `argv` as Lua Table
--- @param  fconfig   {[string]: any}  SWUpdate configuration file's `[suricatta]` section as Lua Table ∪ { polldelay = CHANNEL_DEFAULT_POLLING_INTERVAL}
--- @return suricatta.status           # Suricatta return code
function M.suricatta_funcs.server_start(defaults, argv, fconfig)
    -- Use defaults,
    -- ... shadowed by configuration file values,
    -- ... shadowed by command line arguments.
    local configuration = defaults or {}
    M.utils.table.merge(configuration, fconfig or {})
    local opts = {
        { "url", M.utils.GETOPT_ARG.REQUIRED, "u" },
        { "id", M.utils.GETOPT_ARG.REQUIRED, "i" },
        { "proxy", M.utils.GETOPT_ARG.REQUIRED, "y" },
        { "polldelay", M.utils.GETOPT_ARG.REQUIRED, "p" },
        { "retry", M.utils.GETOPT_ARG.REQUIRED, "r" },
        { "retrywait", M.utils.GETOPT_ARG.REQUIRED, "w" },
        { "confirm", M.utils.GETOPT_ARG.REQUIRED, "c" },
        { "nocheckcert", M.utils.GETOPT_ARG.NO, "x" },
        { nil, M.utils.GETOPT_ARG.NO, "v" },
    }
    for opt, arg in M.utils.getopt(argv or {}, opts) do
        if opt == "u" then
            configuration.url = tostring(arg)
        elseif opt == "i" then
            configuration.id = tostring(arg)
        elseif opt == "y" then
            if not arg then
                io.stderr:write("ERROR: proxy parameter is not a valid string.\n")
                return suricatta.status.EINIT
            end
            configuration.proxy = tostring(arg):gsub('["\']', '')
            if #configuration.proxy == 0 then
                configuration.proxy = suricatta.channel.USE_PROXY_ENV
                if
                    not os.getenv("http_proxy")
                    and not os.getenv("https_proxy")
                    and not os.getenv("HTTPS_PROXY")
                    and not os.getenv("ALL_PROXY")
                then
                    io.stderr:write("ERROR: Should use proxy but no proxy environment variables nor proxy URL set.\n")
                    return suricatta.status.EINIT
                end
            end
        elseif opt == "p" then
            configuration.polldelay = tonumber(arg) or configuration.polldelay
        elseif opt == "r" then
            configuration.retries = tonumber(arg) or configuration.retries
        elseif opt == "w" then
            configuration.retry_sleep = tonumber(arg) or configuration.retry_sleep
        elseif opt == "c" then
            -- Legacy support for suricatta hawkBit's -c encoding
            if arg:lower() == "ok" or arg == suricatta.pstate.TESTING then
                configuration.pstate = suricatta.pstate.TESTING
            elseif arg:lower() == "failed" or arg == suricatta.pstate.FAILED then
                configuration.pstate = suricatta.pstate.FAILED
            else
                io.stderr:write(("ERROR: Invalid argument: -%s %s\n"):format(opt, arg))
                return suricatta.status.EINIT
            end
        elseif opt == "x" then
            configuration.strictssl = false
        elseif opt == "v" then
            configuration.debug = true
        elseif opt == ":" then
            io.stderr:write("ERROR: Missing argument.\n")
            -- Note: .polldelay is in fconfig, make defaults __index it as well.
            M.suricatta_funcs.print_help(setmetatable((defaults or {}), { __index = fconfig }))
            return suricatta.status.EINIT
        end
    end

    for k, v in pairs(configuration) do
        if suricatta.channel.options[k] ~= nil and type(suricatta.channel.options[k]) ~= type(v) then
            suricatta.notify.error(
                "Configuration type mismatch: '%s' must be %s, is %s.",
                k,
                type(suricatta.channel.options[k]),
                type(v)
            )
            return suricatta.status.EINIT
        end
    end

    if not configuration.url or type(configuration.url) ~= "string" then
        suricatta.notify.error("Mandatory parameter missing/mis-typed: URL")
        return suricatta.status.EINIT
    end
    if not configuration.id or type(configuration.id) ~= "string" then
        suricatta.notify.error("Mandatory parameter missing/mis-typed: Device ID")
        return suricatta.status.EINIT
    end
    M.device.id = configuration.id

    if not configuration.pstate then
        M.device.pstate = suricatta.pstate.get()
        if M.device.pstate == suricatta.pstate.ERROR then
            suricatta.notify.error("Error reading persistent state information.")
            return suricatta.status.EINIT
        end
        suricatta.notify.info(
            "Read persistent state %s = %s.",
            string.char(M.device.pstate),
            suricatta.pstate[M.device.pstate] or "N/A"
        )
    else
        M.device.pstate = configuration.pstate
        suricatta.notify.info(
            "Got supplied persistent state %s = %s.",
            string.char(M.device.pstate),
            suricatta.pstate[M.device.pstate] or "N/A"
        )
    end

    M.server.polldelay = configuration.polldelay

    M.channel.main = M.channel(M.utils.table.merge({
        -- An API Gateway may use this information to do steering.
        headers_to_send = {
            ["X-Client-Version"] = M.device.version,
            ["X-Client-Id"] = M.device.id,
        },
    }, configuration))
    if not M.channel.main then
        suricatta.notify.error("Cannot initialize channel.")
        return suricatta.status.EINIT
    end

    local url = ("%s/swagger.json"):format(M.channel.main.options.url)
    suricatta.notify.debug("Suricatta querying %q", url)
    repeat
        local res, _, data = M.channel.main.get { url = url }
        if not res then
            suricatta.notify.warn(
                "Got HTTP error code %d, sleeping %d seconds ...",
                data.http_response_code,
                M.channel.main.options.retry_sleep
            )
            suricatta.sleep(M.channel.main.options.retry_sleep)
        end
    until res and M.job.status:update_json_schema(((data or {}).json_reply or {}))

    suricatta.notify.info("Running with device ID %q on %q", M.device.id, configuration.url)
    return suricatta.status.OK
end
suricatta.server.register(M.suricatta_funcs.server_start, suricatta.server.SERVER_START)

--- Stop the Suricatta server.
--
-- Lua counterpart of `server_op_res_t server_stop(void)`.
--
--- @return suricatta.status  # Suricatta return code
function M.suricatta_funcs.server_stop()
    for name, chan in pairs(M.channel) do
        chan.close()
        M.channel[name] = nil
    end
    M.device:reset()
    M.server:reset()
    return suricatta.status.OK
end
suricatta.server.register(M.suricatta_funcs.server_stop, suricatta.server.SERVER_STOP)

--- Get wfx polling interval.
--
-- Lua counterpart of `unsigned int server_get_polling_interval(void)`.
--
--- @return number  # Polling interval in seconds
function M.suricatta_funcs.get_polling_interval()
    -- Not implemented.
    -- Remotely setting the polling interval is the original concern of
    -- a device management solution that should set the polling interval
    -- via SWUpdate IPC (or command line or configuration file).
    return M.server.polldelay
end
suricatta.server.register(M.suricatta_funcs.get_polling_interval, suricatta.server.GET_POLLING_INTERVAL)

--- Report device configuration/data to server.
--
-- Lua counterpart of `server_op_res_t server_send_target_data(void)`.
--
--- @return suricatta.status  # Suricatta return code
function M.suricatta_funcs.send_target_data()
    -- Not implemented.
    -- This is the original concern of a device management solution that
    -- should report device status including data gathered from SWUpdate
    -- via IPC or the progress interface.
    return suricatta.status.OK
end
suricatta.server.register(M.suricatta_funcs.send_target_data, suricatta.server.SEND_TARGET_DATA)

--- Handle IPC messages sent to Suricatta Lua module.
--
-- Lua counterpart of `server_op_res_t server_ipc(ipc_message *msg)`.
--
--- @param  message  suricatta.ipc.ipc_message  The IPC message sent
--- @return string                              # IPC JSON reply string
--- @return suricatta.status                    # Suricatta return code
function M.suricatta_funcs.ipc(message)
    message = message or {}
    if not message.json then
        return M.utils.string.escape([[{ "request": "IPC requests must be JSON formatted" }]], { ['"'] = '"' }),
            suricatta.status.EBADMSG
    end
    message.msg = message.msg or "<NONE>"
    if message.cmd == message.commands.CONFIG then
        suricatta.notify.debug("Got IPC configuration message: %s", message.msg)
        if message.json.polling then
            M.server.polldelay = tonumber(message.json.polling) or M.server.polldelay
            return M.utils.string.escape([[{ "reply": "applied" }]], { ['"'] = '"' }), suricatta.status.OK
        end
    elseif message.cmd == message.commands.ACTIVATION then
        suricatta.notify.debug("Got IPC activation message: %s", message.msg)
        return M.utils.string.escape([[{ "request": "inapplicable" }]], { ['"'] = '"' }), suricatta.status.OK
    end
    suricatta.notify.warn("Got unknown IPC message: %s", message.msg)
    return M.utils.string.escape([[{ "request": "unknown" }]], { ['"'] = '"' }), suricatta.status.EBADMSG
end
suricatta.server.register(M.suricatta_funcs.ipc, suricatta.server.IPC)

return M
