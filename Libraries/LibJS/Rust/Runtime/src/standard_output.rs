/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The standard output that AK's out() and outln() write to: C stdio's stdout, which buffers lines when it is a
//! terminal and whole blocks otherwise. Writes that C++ makes through Core::File::standard_output() instead go past
//! the buffer, after flushing it, as the C++ tools do. Keeping both kinds of writes apart keeps the order in which
//! output reaches a pipe that also gets the standard error the same as with the C++ tools.

use core::cell::RefCell;
use std::io::{self, IsTerminal, Write};

/// What C stdio buffers for a pipe or a file before it writes it.
const BLOCK_BUFFER_SIZE: usize = 64 * 1024;

struct BufferedStandardOutput {
    buffer: Vec<u8>,
    line_buffered: bool,
}

thread_local! {
    static STANDARD_OUTPUT: RefCell<BufferedStandardOutput> = RefCell::new(BufferedStandardOutput {
        buffer: Vec::new(),
        line_buffered: io::stdout().is_terminal(),
    });
}

fn write_to_file_descriptor(bytes: &[u8]) {
    // NB: Like C stdio, this drops what it cannot write, as a full non-blocking pipe makes it.
    let mut stdout = io::stdout().lock();
    let _ = stdout.write_all(bytes);
    let _ = stdout.flush();
}

/// out(): writes `bytes` to the buffer.
pub fn out(bytes: &[u8]) {
    STANDARD_OUTPUT.with(|standard_output| {
        let mut standard_output = standard_output.borrow_mut();
        standard_output.buffer.extend_from_slice(bytes);
        let flushed_length = if standard_output.line_buffered {
            standard_output
                .buffer
                .iter()
                .rposition(|&byte| byte == b'\n')
                .map_or(0, |index| index + 1)
        } else if standard_output.buffer.len() >= BLOCK_BUFFER_SIZE {
            standard_output.buffer.len()
        } else {
            0
        };
        if flushed_length > 0 {
            let flushed: Vec<u8> = standard_output.buffer.drain(..flushed_length).collect();
            drop(standard_output);
            write_to_file_descriptor(&flushed);
        }
    });
}

/// outln(): writes `bytes` and a newline to the buffer.
pub fn outln(bytes: &[u8]) {
    let mut line = Vec::with_capacity(bytes.len() + 1);
    line.extend_from_slice(bytes);
    line.push(b'\n');
    out(&line);
}

/// fflush(stdout).
pub fn flush() {
    let buffered = STANDARD_OUTPUT.with(|standard_output| core::mem::take(&mut standard_output.borrow_mut().buffer));
    if !buffered.is_empty() {
        write_to_file_descriptor(&buffered);
    }
}

/// Writes `bytes` past the buffer, as C++ writes through Core::File::standard_output() after fflush(stdout).
pub fn write_unbuffered(bytes: &[u8]) {
    flush();
    write_to_file_descriptor(bytes);
}

/// A writer past the buffer, like Core::File::standard_output() and Core::File::standard_error(). Each write goes
/// straight to the file descriptor and fails like write(), also on a closed descriptor, which the standard streams of
/// std report as written. The caller flushes the buffer before writing to the standard output with it.
pub struct UnbufferedWriter {
    stream: StandardStream,
}

#[derive(Clone, Copy)]
enum StandardStream {
    Output,
    Error,
}

impl UnbufferedWriter {
    pub const STANDARD_OUTPUT: Self = Self {
        stream: StandardStream::Output,
    };
    pub const STANDARD_ERROR: Self = Self {
        stream: StandardStream::Error,
    };
}

impl Write for UnbufferedWriter {
    #[cfg(unix)]
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let file_descriptor = match self.stream {
            StandardStream::Output => libc::STDOUT_FILENO,
            StandardStream::Error => libc::STDERR_FILENO,
        };
        // SAFETY: The bytes are valid for their length.
        let written = unsafe { libc::write(file_descriptor, bytes.as_ptr().cast(), bytes.len()) };
        usize::try_from(written).map_err(|_| io::Error::last_os_error())
    }

    /// Core::File::write_some() on Windows: WriteFile() on the handle of the stream.
    #[cfg(windows)]
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        use std::os::windows::io::{AsRawHandle, RawHandle};

        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn WriteFile(
                file: RawHandle,
                buffer: *const u8,
                length: u32,
                written: *mut u32,
                overlapped: *mut core::ffi::c_void,
            ) -> i32;
        }

        let handle = match self.stream {
            StandardStream::Output => io::stdout().as_raw_handle(),
            StandardStream::Error => io::stderr().as_raw_handle(),
        };
        let length = u32::try_from(bytes.len()).unwrap_or(u32::MAX);
        let mut written = 0;
        // SAFETY: The bytes are valid for `length` bytes, and WriteFile() fails on a null or closed handle.
        if unsafe { WriteFile(handle, bytes.as_ptr(), length, &raw mut written, core::ptr::null_mut()) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(written as usize)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// A writer into the buffer, for code that writes to a std::io::Write.
pub struct StandardOutputWriter;

impl Write for StandardOutputWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        out(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        flush();
        Ok(())
    }
}
