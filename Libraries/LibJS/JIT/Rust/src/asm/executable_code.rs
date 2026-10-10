/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Machine code in executable memory, for tests that run generated code
//! natively.

/// Machine code copied into its own executable mapping (never writable and
/// executable at the same time).
pub(crate) struct ExecutableCode {
    address: *mut libc::c_void,
    size: usize,
}

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn pthread_jit_write_protect_np(enabled: libc::c_int);
    fn sys_icache_invalidate(start: *mut libc::c_void, length: libc::size_t);
}

#[cfg(not(target_os = "macos"))]
unsafe extern "C" {
    /// The compiler runtime's instruction cache flush (a no-op on x86-64).
    fn __clear_cache(start: *mut libc::c_char, end: *mut libc::c_char);
}

impl ExecutableCode {
    pub(crate) fn new(code: &[u8]) -> Self {
        let size = code.len().max(1).next_multiple_of(16384);
        #[cfg(target_os = "macos")]
        // SAFETY: A fresh private anonymous mapping for JIT code, written
        // while the thread's write protection of it is off.
        unsafe {
            let address = libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE | libc::PROT_EXEC,
                libc::MAP_PRIVATE | libc::MAP_ANON | libc::MAP_JIT,
                -1,
                0,
            );
            assert_ne!(address, libc::MAP_FAILED);
            pthread_jit_write_protect_np(0);
            std::ptr::copy_nonoverlapping(code.as_ptr(), address.cast::<u8>(), code.len());
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(address, code.len());
            Self { address, size }
        }
        #[cfg(not(target_os = "macos"))]
        // SAFETY: A fresh private anonymous mapping, written before it is
        // made executable.
        unsafe {
            let address = libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            );
            assert_ne!(address, libc::MAP_FAILED);
            std::ptr::copy_nonoverlapping(code.as_ptr(), address.cast::<u8>(), code.len());
            assert_eq!(libc::mprotect(address, size, libc::PROT_READ | libc::PROT_EXEC), 0);
            __clear_cache(address.cast(), address.cast::<libc::c_char>().add(code.len()));
            Self { address, size }
        }
    }

    /// Reinterprets the code as a function pointer of type `F`.
    ///
    /// # Safety
    /// `F` must be an `extern "C" fn` type matching what the code implements.
    pub(crate) unsafe fn function<F: Copy>(&self) -> F {
        assert_eq!(size_of::<F>(), size_of::<*mut libc::c_void>());
        // SAFETY: The caller guarantees `F` is a matching function pointer type.
        unsafe { std::mem::transmute_copy(&self.address) }
    }
}

impl Drop for ExecutableCode {
    fn drop(&mut self) {
        // SAFETY: The mapping was created in `new` with this size.
        unsafe {
            libc::munmap(self.address, self.size);
        }
    }
}
