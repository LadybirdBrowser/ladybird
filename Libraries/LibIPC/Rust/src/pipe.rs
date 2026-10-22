/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reads and writes on a Windows pipe, for the Windows transport.
//!
//! Both ends of the pipes that `Core::System::duplex_pipe()` makes are overlapped, so every read and write goes with an
//! `OVERLAPPED` and an event of its own, and waits for its event or for the pipe to be stopped.

#![allow(unsafe_code)]

use std::ffi::c_void;
use std::io;
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle, RawHandle};
use std::sync::atomic::{AtomicBool, Ordering};

const ERROR_BROKEN_PIPE: i32 = 109;
const ERROR_NO_DATA: i32 = 232;
const ERROR_PIPE_NOT_CONNECTED: i32 = 233;
const ERROR_OPERATION_ABORTED: i32 = 995;
const ERROR_IO_PENDING: i32 = 997;
const WAIT_OBJECT_0: u32 = 0;
const INFINITE: u32 = u32::MAX;

/// `OVERLAPPED`, whose offset union is two 32-bit halves wide on every target.
#[repr(C)]
struct Overlapped {
    internal: usize,
    internal_high: usize,
    offset: u32,
    offset_high: u32,
    event: RawHandle,
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateEventW(attributes: *const c_void, manual_reset: i32, initial_state: i32, name: *const u16) -> RawHandle;
    fn SetEvent(event: RawHandle) -> i32;
    fn ReadFile(file: RawHandle, buffer: *mut u8, length: u32, read: *mut u32, overlapped: *mut Overlapped) -> i32;
    fn WriteFile(
        file: RawHandle,
        buffer: *const u8,
        length: u32,
        written: *mut u32,
        overlapped: *mut Overlapped,
    ) -> i32;
    fn GetOverlappedResult(file: RawHandle, overlapped: *mut Overlapped, transferred: *mut u32, wait: i32) -> i32;
    fn CancelIoEx(file: RawHandle, overlapped: *mut Overlapped) -> i32;
    fn WaitForMultipleObjects(count: u32, handles: *const RawHandle, wait_all: i32, milliseconds: u32) -> u32;
}

/// How a read or write ended without moving bytes.
#[derive(Debug)]
pub(crate) enum PipeError {
    /// The peer closed its end.
    Closed,
    /// The pipe was stopped.
    Stopped,
    Io(io::Error),
}

impl PipeError {
    fn from_last_error() -> Self {
        let error = io::Error::last_os_error();
        match error.raw_os_error() {
            Some(ERROR_BROKEN_PIPE | ERROR_NO_DATA | ERROR_PIPE_NOT_CONNECTED) => Self::Closed,
            Some(ERROR_OPERATION_ABORTED) => Self::Stopped,
            _ => Self::Io(error),
        }
    }
}

fn create_event() -> io::Result<OwnedHandle> {
    // SAFETY: This makes a manual-reset, unnamed event with default security.
    let event = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
    if event.is_null() {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: The event is new, and nothing else owns it.
    Ok(unsafe { OwnedHandle::from_raw_handle(event) })
}

/// The event that the reads or the writes of one thread wait on.
pub(crate) struct Operation {
    event: OwnedHandle,
}

impl Operation {
    pub fn new() -> io::Result<Self> {
        Ok(Self { event: create_event()? })
    }
}

/// An overlapped pipe handle, which reads and writes until it is stopped.
pub(crate) struct Pipe {
    handle: OwnedHandle,
    stop_event: OwnedHandle,
    stopped: AtomicBool,
}

impl Pipe {
    pub fn new(handle: OwnedHandle) -> io::Result<Self> {
        Ok(Self {
            handle,
            stop_event: create_event()?,
            stopped: AtomicBool::new(false),
        })
    }

    /// Ends the reads and writes that wait now, and refuses those that come after.
    pub fn stop(&self) {
        self.stopped.store(true, Ordering::Release);
        // SAFETY: The event lives as long as the pipe.
        unsafe { SetEvent(self.stop_event.as_raw_handle()) };
    }

    /// Reads at least one byte, unless the pipe ends first.
    pub fn read(&self, operation: &Operation, buffer: &mut [u8]) -> Result<usize, PipeError> {
        let length = u32::try_from(buffer.len()).unwrap_or(u32::MAX);
        let buffer = buffer.as_mut_ptr();
        self.transfer(operation, |handle, overlapped| {
            // SAFETY: The buffer holds `length` bytes, and outlives the read, which `transfer` waits for.
            unsafe { ReadFile(handle, buffer, length, std::ptr::null_mut(), overlapped) }
        })
    }

    /// Writes some of `bytes`, unless the pipe ends first.
    pub fn write(&self, operation: &Operation, bytes: &[u8]) -> Result<usize, PipeError> {
        let length = u32::try_from(bytes.len()).unwrap_or(u32::MAX);
        let bytes = bytes.as_ptr();
        self.transfer(operation, |handle, overlapped| {
            // SAFETY: The bytes are valid for `length` bytes, and outlive the write, which `transfer` waits for.
            unsafe { WriteFile(handle, bytes, length, std::ptr::null_mut(), overlapped) }
        })
    }

    fn transfer(
        &self,
        operation: &Operation,
        start: impl FnOnce(RawHandle, *mut Overlapped) -> i32,
    ) -> Result<usize, PipeError> {
        if self.stopped.load(Ordering::Acquire) {
            return Err(PipeError::Stopped);
        }
        let handle = self.handle.as_raw_handle();
        // NB: The operation may use this OVERLAPPED until it is over, so every path below waits for that before
        //     returning.
        let mut overlapped = Overlapped {
            internal: 0,
            internal_high: 0,
            offset: 0,
            offset_high: 0,
            event: operation.event.as_raw_handle(),
        };
        if start(handle, &raw mut overlapped) == 0 {
            let error = PipeError::from_last_error();
            if !matches!(&error, PipeError::Io(error) if error.raw_os_error() == Some(ERROR_IO_PENDING)) {
                return Err(error);
            }
            let events = [operation.event.as_raw_handle(), self.stop_event.as_raw_handle()];
            // SAFETY: Both events are live.
            let signaled = unsafe { WaitForMultipleObjects(2, events.as_ptr(), 0, INFINITE) };
            if signaled != WAIT_OBJECT_0 {
                let mut transferred = 0;
                // SAFETY: The operation was started on this handle with this OVERLAPPED, which stays put until the
                //         cancelled operation is over.
                unsafe {
                    CancelIoEx(handle, &raw mut overlapped);
                    GetOverlappedResult(handle, &raw mut overlapped, &raw mut transferred, 1);
                }
                return Err(PipeError::Stopped);
            }
        }
        let mut transferred = 0;
        // SAFETY: The operation is over, whether it finished at once or its event was signaled.
        if unsafe { GetOverlappedResult(handle, &raw mut overlapped, &raw mut transferred, 0) } == 0 {
            return Err(PipeError::from_last_error());
        }
        Ok(transferred as usize)
    }
}
