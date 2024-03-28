use std::os::raw::{c_char, c_int, c_uint};

use crate::types::source_type::SourceType;
use crate::SwupdateRequest;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Status {
    pub current: c_int,
    last_result: c_int,
    error: c_int,
    pub desc: [c_char; 2048],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Notify {
    status: c_int,
    error: c_int,
    level: c_int,
    msg: [c_char; 2048],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InstMsg {
    req: SwupdateRequest,
    len: c_uint,
    buf: [c_char; 2048],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcMsg {
    source: SourceType,
    cmd: c_int,
    timeout: c_int,
    len: c_uint,
    buf: [c_char; 2048],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AESKeyMsg {
    key_ascii: [c_char; 65],
    ivt_ascii: [c_char; 33],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Versions {
    minimum_version: [c_char; 256],
    maximum_version: [c_char; 256],
    current_version: [c_char; 256],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Revisions {
    boardname: [c_char; 256],
    revision: [c_char; 256],
}

#[repr(C)]
pub union MsgData {
    msg: [c_char; 128],
    pub status: Status,
    notify: Notify,
    inst_msg: InstMsg,
    proc_msg: ProcMsg,
    aes_key_msg: AESKeyMsg,
    versions: Versions,
    revisions: Revisions,
}

impl Default for MsgData {
    fn default() -> Self {
        MsgData { msg: [0; 128] }
    }
}

#[repr(C)]
pub struct IpcMessage {
    magic: c_int,
    type_: c_int,
    pub data: MsgData,
}
