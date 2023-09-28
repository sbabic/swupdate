use std::os::raw::c_char;

use crate::types::{run_type::RunType, source_type::SourceType};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwupdateRequest {
    pub apiversion: u32,
    pub source: SourceType,
    pub dry_run: RunType,
    pub len: usize,
    pub info: [c_char; 512],
    pub software_set: [c_char; 256],
    pub running_mode: [c_char; 256],
    pub disable_store_swu: bool,
}
