// Safe Rust wrapper over the frozen Shuttle C ABI v1 (consumer subset).
//
// Extern declarations are hand-written against include/shuttle/shuttle_c.h
// (ABI v1, frozen; a drift would fail the byte-exact integration test).
// bindgen is deliberately not used here: it would drag libclang into the
// container for ten stable signatures — recorded as a ledger decision.
//
// THE LIFETIME CONTRACT (the point of G6.2): `acquire_read` borrows the
// Consumer mutably and returns `Borrowed<'_>`; the payload slice from
// `as_slice` is tied to the Borrowed's lifetime, and Drop performs
// shuttle_release_read. Consequences enforced AT COMPILE TIME:
//   - the slice cannot outlive the release (use-after-release = E0597),
//   - no second acquire while a borrow is outstanding (Consumer is
//     mutably borrowed until the Borrowed drops).
#![allow(dead_code)]

use std::ffi::CString;
use std::marker::PhantomData;
use std::os::raw::{c_char, c_int, c_void};

#[repr(C)]
pub struct ShuttleChannel {
    _opaque: [u8; 0],
}

extern "C" {
    fn shuttle_open(name: *const c_char, err: *mut c_int) -> *mut ShuttleChannel;
    fn shuttle_close(ch: *mut ShuttleChannel);
    fn shuttle_acquire_read(
        ch: *mut ShuttleChannel,
        ptr: *mut *const c_void,
        len: *mut usize,
        flags: c_int,
    ) -> c_int;
    fn shuttle_release_read(ch: *mut ShuttleChannel) -> c_int;
}

pub struct Consumer {
    ch: *mut ShuttleChannel,
}

pub struct Borrowed<'a> {
    ptr: *const u8,
    len: usize,
    ch: *mut ShuttleChannel,
    _consumer: PhantomData<&'a mut Consumer>,
}

impl Consumer {
    pub fn open(name: &str) -> Result<Consumer, i32> {
        let cname = CString::new(name).map_err(|_| -1)?;
        let mut err: c_int = 0;
        let ch = unsafe { shuttle_open(cname.as_ptr(), &mut err) };
        if ch.is_null() {
            return Err(err as i32);
        }
        Ok(Consumer { ch })
    }

    // Blocking zero-copy borrow of the next message.
    pub fn acquire_read(&mut self) -> Result<Borrowed<'_>, i32> {
        let mut ptr: *const c_void = std::ptr::null();
        let mut len: usize = 0;
        let rc = unsafe { shuttle_acquire_read(self.ch, &mut ptr, &mut len, 0) };
        if rc != 0 {
            return Err(rc as i32);
        }
        Ok(Borrowed {
            ptr: ptr as *const u8,
            len,
            ch: self.ch,
            _consumer: PhantomData,
        })
    }
}

impl Drop for Consumer {
    fn drop(&mut self) {
        unsafe { shuttle_close(self.ch) };
    }
}

impl<'a> Borrowed<'a> {
    // Zero-copy view; the returned slice cannot outlive this Borrowed.
    pub fn as_slice(&self) -> &[u8] {
        if self.len == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }

    pub fn len(&self) -> usize {
        self.len
    }
}

impl<'a> Drop for Borrowed<'a> {
    fn drop(&mut self) {
        unsafe { shuttle_release_read(self.ch) };
    }
}
