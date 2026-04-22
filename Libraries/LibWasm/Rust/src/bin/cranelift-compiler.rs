/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::manual_let_else)]

use libwasm_cranelift::{CraneliftInsn, RuntimeHelpers, compile_to_bytes};
use std::env;
use std::mem::{size_of, size_of_val};

#[cfg(unix)]
use std::fs::File;
#[cfg(unix)]
use std::io;
#[cfg(unix)]
use std::os::fd::FromRawFd;
#[cfg(unix)]
use std::os::unix::fs::FileExt;

#[repr(C)]
#[derive(Clone, Copy)]
struct InputHeader {
    function_count: u32,
    helpers_offset: u32,
    outcome_return: u64,
    code_region_start: u64,
    total_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InputFunctionEntry {
    insn_offset: u32,
    insn_count: u32,
    result_arity: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct OutputFunctionEntry {
    code_offset: u64,
    code_size: u32,
    compiled: u32,
}

fn as_bytes_slice<T>(value: &[T]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(value.as_ptr().cast::<u8>(), size_of_val(value)) }
}

fn read_pod<T: Copy>(base: &[u8], offset: usize) -> Result<T, &'static str> {
    let end = offset.checked_add(size_of::<T>()).ok_or("overflow")?;
    let bytes = base.get(offset..end).ok_or("out of bounds read")?;
    Ok(unsafe { (bytes.as_ptr().cast::<T>()).read_unaligned() })
}

#[cfg(unix)]
fn read_exact_at_offset(file: &File, buf: &mut [u8], offset: u64) -> io::Result<()> {
    file.read_exact_at(buf, offset)
}

#[cfg(unix)]
fn write_all_at_offset(file: &File, data: &[u8], offset: u64) -> io::Result<()> {
    file.write_all_at(data, offset)
}

#[cfg(windows)]
mod win {
    use std::ffi::c_void;
    use std::io;

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
        pub fn open(arg: &str) -> Result<Self, Box<dyn std::error::Error>> {
            let handle_val = arg.parse::<usize>()?;
            let handle = handle_val as *mut c_void;
            let ptr = unsafe { MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0) };
            if ptr.is_null() {
                return Err(io::Error::last_os_error().into());
            }
            let header = unsafe { ptr.cast::<super::InputHeader>().read_unaligned() };
            let len = usize::try_from(header.total_size).map_err(|_| "size overflow")?;
            Ok(Mapping {
                ptr: ptr.cast::<u8>(),
                len,
            })
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
    let arg = env::args()
        .nth(1)
        .ok_or("Usage: cranelift-compiler <shm-fd-or-handle>")?;

    #[cfg(unix)]
    let (file, mut owned_mapped): (File, Vec<u8>) = {
        let shmfd = arg.parse::<i32>()?;
        if shmfd < 0 {
            return Err("invalid fd".into());
        }
        let file = unsafe { File::from_raw_fd(shmfd) };

        let mut header_buf = [0u8; size_of::<InputHeader>()];
        read_exact_at_offset(&file, &mut header_buf, 0)?;
        let header = unsafe { (header_buf.as_ptr().cast::<InputHeader>()).read_unaligned() };

        let total_size = usize::try_from(header.total_size).map_err(|_| "total_size overflow")?;
        let mut buf = vec![0u8; total_size];
        read_exact_at_offset(&file, &mut buf, 0)?;
        (file, buf)
    };

    #[cfg(windows)]
    let mut mapping = win::Mapping::open(&arg)?;

    #[cfg(unix)]
    let mapped: &mut [u8] = &mut owned_mapped;
    #[cfg(windows)]
    let mapped: &mut [u8] = mapping.as_slice_mut();

    let header: InputHeader = read_pod(mapped, 0)?;
    let func_count = usize::try_from(header.function_count).map_err(|_| "function_count overflow")?;
    let entries_offset = size_of::<InputHeader>();
    let helpers_offset = usize::try_from(header.helpers_offset).map_err(|_| "helpers_offset overflow")?;
    let code_region_start = usize::try_from(header.code_region_start).map_err(|_| "code_region_start overflow")?;
    let helpers: RuntimeHelpers = read_pod(mapped, helpers_offset)?;

    let out_entries_offset = code_region_start;
    let code_base_offset = out_entries_offset + func_count * size_of::<OutputFunctionEntry>();
    let code_capacity = mapped.len().checked_sub(code_base_offset).ok_or("bad code region")?;

    let mut entries: Vec<InputFunctionEntry> = Vec::with_capacity(func_count);
    for i in 0..func_count {
        let entry_offset = entries_offset + i * size_of::<InputFunctionEntry>();
        entries.push(read_pod(mapped, entry_offset)?);
    }

    let thread_count = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .max(1);
    let chunk_size = func_count.div_ceil(thread_count.max(1));
    let mapped_ref: &[u8] = mapped;
    let helpers_ref = &helpers;
    let outcome_return = header.outcome_return;

    let compiled_chunks: Vec<Vec<(usize, Vec<u8>)>> = std::thread::scope(|scope| {
        let mut handles = Vec::with_capacity(thread_count);
        for chunk_idx in 0..thread_count {
            let start = chunk_idx * chunk_size;
            if start >= func_count {
                break;
            }
            let end = (start + chunk_size).min(func_count);
            let chunk_entries = &entries[start..end];
            handles.push(scope.spawn(move || {
                let mut out: Vec<(usize, Vec<u8>)> = Vec::with_capacity(end - start);
                for (offset_in_chunk, entry) in chunk_entries.iter().enumerate() {
                    let i = start + offset_in_chunk;
                    if entry.insn_count == 0 {
                        continue;
                    }
                    let insn_offset = match usize::try_from(entry.insn_offset) {
                        Ok(v) => v,
                        Err(_) => continue,
                    };
                    let insn_count = entry.insn_count as usize;
                    let insn_bytes_len = match insn_count.checked_mul(size_of::<CraneliftInsn>()) {
                        Some(v) => v,
                        None => continue,
                    };
                    let insn_bytes = match mapped_ref.get(insn_offset..insn_offset + insn_bytes_len) {
                        Some(b) => b,
                        None => continue,
                    };
                    let insns =
                        unsafe { std::slice::from_raw_parts(insn_bytes.as_ptr().cast::<CraneliftInsn>(), insn_count) };
                    if let Ok(code) = compile_to_bytes(insns, helpers_ref, outcome_return, entry.result_arity) {
                        out.push((i, code));
                    }
                }
                out
            }));
        }
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });

    let mut code_cursor = 0usize;
    for chunk in compiled_chunks {
        for (i, code) in chunk {
            let aligned = (code.len() + 15) & !15;
            if code_cursor + aligned > code_capacity {
                continue;
            }
            let code_offset = code_cursor;
            let code_dst = code_base_offset + code_offset;
            mapped[code_dst..code_dst + code.len()].copy_from_slice(&code);

            let entry = OutputFunctionEntry {
                code_offset: u64::try_from(code_offset).map_err(|_| "code offset overflow")?,
                code_size: u32::try_from(code.len()).map_err(|_| "code size overflow")?,
                compiled: 1,
            };
            let entry_dst = out_entries_offset + i * size_of::<OutputFunctionEntry>();
            let entry_bytes = as_bytes_slice(std::slice::from_ref(&entry));
            mapped[entry_dst..entry_dst + size_of::<OutputFunctionEntry>()].copy_from_slice(entry_bytes);

            code_cursor += aligned;
        }
    }

    #[cfg(unix)]
    {
        write_all_at_offset(
            &file,
            &mapped[out_entries_offset..code_base_offset],
            u64::try_from(out_entries_offset)?,
        )?;
        write_all_at_offset(
            &file,
            &mapped[code_base_offset..code_base_offset + code_cursor],
            u64::try_from(code_base_offset)?,
        )?;
        let _ = file.sync_all();
    }

    Ok(())
}
