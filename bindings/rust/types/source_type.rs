#[repr(C)]
#[derive(Clone, Copy)]
pub enum SourceType {
    _Unknown,
    _Webserver,
    _Suricatta,
    _Downloader,
    Local,
    _ChunksDownloader,
}
