/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::manual_let_else)]

use libwasm_cranelift::serialized::compile_serialized_buffer;
use std::env;

// macOS POSIX shm objects don't support read/write/pread/pwrite, only mmap and
// ftruncate. Mmap both buffers on all Unix platforms so Linux does not need to
// copy them through temporary Vecs.
#[cfg(unix)]
mod unix {
    use std::ffi::c_void;
    use std::io;

    pub struct Mapping {
        ptr: *mut u8,
        len: usize,
        fd: i32,
    }

    impl Mapping {
        pub fn open(fd: i32, len: usize, writable: bool) -> Result<Self, Box<dyn std::error::Error>> {
            if fd < 0 {
                return Err("invalid fd".into());
            }
            let protection = if writable {
                libc::PROT_READ | libc::PROT_WRITE
            } else {
                libc::PROT_READ
            };
            let ptr = unsafe { libc::mmap(std::ptr::null_mut(), len, protection, libc::MAP_SHARED, fd, 0) };
            if ptr == libc::MAP_FAILED {
                return Err(io::Error::last_os_error().into());
            }
            Ok(Mapping {
                ptr: ptr.cast::<u8>(),
                len,
                fd,
            })
        }

        pub fn as_slice(&self) -> &[u8] {
            unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
        }

        pub fn as_slice_mut(&mut self) -> &mut [u8] {
            unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
        }
    }

    impl Drop for Mapping {
        fn drop(&mut self) {
            unsafe {
                libc::munmap(self.ptr.cast::<c_void>(), self.len);
                libc::close(self.fd);
            }
        }
    }
}

#[cfg(windows)]
mod win {
    use std::ffi::c_void;
    use std::io;

    const FILE_MAP_READ: u32 = 0x0004;
    const FILE_MAP_ALL_ACCESS: u32 = 0xf001f;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        // https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile
        fn MapViewOfFile(
            hFileMappingObject: *mut c_void, // HANDLE
            dwDesiredAccess: u32,            // DWORD
            dwFileOffsetHigh: u32,           // DWORD
            dwFileOffsetLow: u32,            // DWORD
            dwNumberOfBytesToMap: usize,     // size_t
        ) -> *mut c_void; // LPVOID

        // https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile
        fn UnmapViewOfFile(lpBaseAddress: *const c_void, // LPCVOID
        ) -> i32; // BOOL
    }

    pub struct Mapping {
        ptr: *mut u8,
        len: usize,
    }

    impl Mapping {
        pub fn open(arg: &str, len: usize, writable: bool) -> Result<Self, Box<dyn std::error::Error>> {
            let handle_val = arg.parse::<usize>()?;
            let handle = handle_val as *mut c_void;
            let desired_access = if writable { FILE_MAP_ALL_ACCESS } else { FILE_MAP_READ };
            let ptr = unsafe { MapViewOfFile(handle, desired_access, 0, 0, len) };
            if ptr.is_null() {
                return Err(io::Error::last_os_error().into());
            }
            Ok(Mapping {
                ptr: ptr.cast::<u8>(),
                len,
            })
        }

        pub fn as_slice(&self) -> &[u8] {
            unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
        }

        pub fn as_slice_mut(&mut self) -> &mut [u8] {
            unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
        }
    }

    impl Drop for Mapping {
        fn drop(&mut self) {
            unsafe {
                UnmapViewOfFile(self.ptr.cast());
            }
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = env::args().skip(1);
    let input_handle = arguments
        .next()
        .ok_or("Usage: cranelift-compiler <input-handle> <input-size> <output-handle> <output-size>")?;
    let input_size = arguments.next().ok_or("missing input size")?.parse::<usize>()?;
    let output_handle = arguments.next().ok_or("missing output handle")?;
    let output_size = arguments.next().ok_or("missing output size")?.parse::<usize>()?;

    #[cfg(unix)]
    let input = unix::Mapping::open(input_handle.parse::<i32>()?, input_size, false)?;
    #[cfg(windows)]
    let input = win::Mapping::open(&input_handle, input_size, false)?;

    #[cfg(unix)]
    let mut output = unix::Mapping::open(output_handle.parse::<i32>()?, output_size, true)?;
    #[cfg(windows)]
    let mut output = win::Mapping::open(&output_handle, output_size, true)?;

    compile_serialized_buffer(input.as_slice(), output.as_slice_mut())?;

    Ok(())
}
