/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The descriptor of a shared memory object, as Core::AnonymousBuffer passes it as an int: a file descriptor, or on
//! Windows a HANDLE that AK's to_fd() narrowed.

use core::ffi::c_int;

#[cfg(unix)]
pub type OwnedSharedMemory = std::os::fd::OwnedFd;
#[cfg(unix)]
pub type BorrowedSharedMemory<'descriptor> = std::os::fd::BorrowedFd<'descriptor>;

#[cfg(windows)]
pub type OwnedSharedMemory = std::os::windows::io::OwnedHandle;
#[cfg(windows)]
pub type BorrowedSharedMemory<'descriptor> = std::os::windows::io::BorrowedHandle<'descriptor>;

/// AsFd, or AsHandle on Windows.
pub trait AsSharedMemory {
    fn as_shared_memory(&self) -> BorrowedSharedMemory<'_>;
}

#[cfg(unix)]
impl<T: std::os::fd::AsFd> AsSharedMemory for T {
    fn as_shared_memory(&self) -> BorrowedSharedMemory<'_> {
        self.as_fd()
    }
}

#[cfg(windows)]
impl<T: std::os::windows::io::AsHandle> AsSharedMemory for T {
    fn as_shared_memory(&self) -> BorrowedSharedMemory<'_> {
        self.as_handle()
    }
}

#[cfg(windows)]
fn handle_from_descriptor(descriptor: c_int) -> std::os::windows::io::RawHandle {
    // AK's to_handle() sign-extends the int.
    descriptor as isize as std::os::windows::io::RawHandle
}

#[cfg(windows)]
fn descriptor_from_handle(handle: std::os::windows::io::RawHandle) -> c_int {
    // AK's to_fd() keeps the low 32 bits, which hold every value a kernel handle can have.
    handle as isize as c_int
}

pub fn raw_descriptor(shared_memory: BorrowedSharedMemory<'_>) -> c_int {
    #[cfg(unix)]
    {
        std::os::fd::AsRawFd::as_raw_fd(&shared_memory)
    }
    #[cfg(windows)]
    {
        descriptor_from_handle(std::os::windows::io::AsRawHandle::as_raw_handle(&shared_memory))
    }
}

pub fn into_raw_descriptor(shared_memory: OwnedSharedMemory) -> c_int {
    #[cfg(unix)]
    {
        std::os::fd::IntoRawFd::into_raw_fd(shared_memory)
    }
    #[cfg(windows)]
    {
        descriptor_from_handle(std::os::windows::io::IntoRawHandle::into_raw_handle(shared_memory))
    }
}

/// # Safety
///
/// `descriptor` must be open, and owned by nothing else.
pub unsafe fn owned_from_raw_descriptor(descriptor: c_int) -> OwnedSharedMemory {
    #[cfg(unix)]
    {
        // SAFETY: As the caller guarantees.
        unsafe { std::os::fd::FromRawFd::from_raw_fd(descriptor) }
    }
    #[cfg(windows)]
    {
        // SAFETY: As the caller guarantees.
        unsafe { std::os::windows::io::FromRawHandle::from_raw_handle(handle_from_descriptor(descriptor)) }
    }
}

/// # Safety
///
/// `descriptor` must stay open for `'descriptor`.
pub unsafe fn borrowed_from_raw_descriptor<'descriptor>(descriptor: c_int) -> BorrowedSharedMemory<'descriptor> {
    #[cfg(unix)]
    {
        // SAFETY: As the caller guarantees.
        unsafe { BorrowedSharedMemory::borrow_raw(descriptor) }
    }
    #[cfg(windows)]
    {
        // SAFETY: As the caller guarantees.
        unsafe { BorrowedSharedMemory::borrow_raw(handle_from_descriptor(descriptor)) }
    }
}
