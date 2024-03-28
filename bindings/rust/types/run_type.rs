#[repr(C)]
#[derive(Clone, Copy)]
pub enum RunType {
    Default,
    Dryrun,
    _Install,
}
