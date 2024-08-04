use lazy_static::lazy_static;
use once_cell::sync::OnceCell;
use std::ffi::{CStr, CString};
use std::io::{Error, ErrorKind};
use std::os::raw::{c_char, c_int};
use std::sync::{Condvar, Mutex};

mod types;
use types::{
    ipc_message::IpcMessage, run_type::RunType, source_type::SourceType,
    swupdate_request::SwupdateRequest,
};

type WriteData = unsafe extern "C" fn(*mut *mut c_char, *mut c_int) -> c_int;
type GetStatus = unsafe extern "C" fn(*mut IpcMessage) -> c_int;
type Terminated = unsafe extern "C" fn(c_int) -> c_int;
type Callback = fn(String);

lazy_static! {
    static ref MY_MUTEX: Mutex<bool> = Mutex::new(false);
    static ref CV_END: Condvar = Condvar::new();
}

static mut FD: c_int = 0;
static mut BUF: [u8; 256] = [0u8; 256];
static mut CB: OnceCell<Callback> = OnceCell::new();

#[link(name = "swupdate")]
extern "C" {
    fn swupdate_async_start(
        wr_func: WriteData,
        status_func: GetStatus,
        end_func: Terminated,
        req: *mut SwupdateRequest,
        size: isize,
    ) -> c_int;

    fn swupdate_prepare_req(req: *mut SwupdateRequest);
}

extern "C" fn readimage(p: *mut *mut c_char, size: *mut c_int) -> c_int {
    unsafe {
        let ret = libc::read(FD, BUF.as_mut_ptr() as *mut std::ffi::c_void, BUF.len());

        *p = BUF.as_mut_ptr() as *mut c_char;
        *size = ret as c_int;

        ret as c_int
    }
}

extern "C" fn printstatus(msg: *mut IpcMessage) -> c_int {
    unsafe {
        CB.get().expect("Callback not initialized")(format!(
            "Status: {:?} message: {:?}\n",
            (*msg).data.status.current,
            CStr::from_ptr((*msg).data.status.desc.as_ptr())
        ));
    };
    0
}

extern "C" fn end(_status: c_int) -> c_int {
    let mut guard = MY_MUTEX.lock().unwrap();
    *guard = true;
    CV_END.notify_one();

    0
}

fn open_file(filename: &str) -> c_int {
    let c_filename = CString::new(filename).expect("CString::new failed");
    unsafe {
        let fd = libc::open(c_filename.as_ptr(), libc::O_RDONLY);
        if fd < 0 {
            eprintln!("Unable to open {}", filename);
            return libc::EXIT_FAILURE;
        }
        FD = fd;
    }
    libc::EXIT_SUCCESS
}

fn close_file() {
    unsafe {
        if FD >= 0 {
            libc::close(FD);
        }
    }
}

pub fn send_file(filename: &str, dry_run: bool, cb: Callback) -> Result<(), Error> {
    open_file(filename);

    let dry_run_val = if dry_run {
        RunType::Dryrun
    } else {
        RunType::Default
    };

    let mut req = SwupdateRequest {
        apiversion: 0,
        source: SourceType::Local,
        dry_run: dry_run_val,
        len: 0,
        info: [0; 512],
        software_set: [0; 256],
        running_mode: [0; 256],
        disable_store_swu: false,
    };

    unsafe { swupdate_prepare_req(&mut req) };

    let mut guard = MY_MUTEX.lock().unwrap();

    unsafe {
        let size = std::mem::size_of_val(&req).try_into().unwrap();
        CB.get_or_init(|| cb);
        let rc = swupdate_async_start(readimage, printstatus, end, &mut req, size);
        if rc < 0 {
            close_file();
            return Err(Error::new(
                ErrorKind::Other,
                format!(
                    "Software update failed to start with return code: {:?}",
                    libc::EXIT_FAILURE
                ),
            ));
        }
    }

    while !*guard {
        guard = CV_END.wait(guard).unwrap();
    }

    close_file();

    Ok(())
}
