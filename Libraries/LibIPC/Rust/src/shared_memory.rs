/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Creating anonymous files for socket transport payloads.

#![allow(unsafe_code)]

use std::io;
use std::os::fd::{FromRawFd, OwnedFd};

#[cfg(target_os = "linux")]
pub(crate) fn create_anonymous_file() -> io::Result<OwnedFd> {
    // NB: memfd_create() reads a NUL-terminated name and returns a new descriptor.
    let fd = unsafe { libc::memfd_create(c"".as_ptr(), libc::MFD_CLOEXEC) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    // NB: The descriptor is new, and nothing else owns it.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

#[cfg(not(target_os = "linux"))]
pub(crate) fn create_anonymous_file() -> io::Result<OwnedFd> {
    use std::hash::{BuildHasher, RandomState};
    use std::os::fd::AsRawFd;

    // NB: Create the shared memory object under an unpredictable, single-use name, and unlink it immediately, so only the
    //     returned descriptor keeps it alive. O_EXCL makes the open fail if the name already exists, so try another one.
    for attempt in 0u32..16 {
        let random = RandomState::new().hash_one((std::process::id(), std::time::Instant::now(), attempt));
        let name = std::ffi::CString::new(format!("/shm-{random:016x}"))
            .map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;
        // NB: shm_open() reads a NUL-terminated name and returns a new descriptor.
        let fd = unsafe { libc::shm_open(name.as_ptr(), libc::O_RDWR | libc::O_CREAT | libc::O_EXCL, 0o600) };
        if fd < 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(libc::EEXIST) {
                continue;
            }
            return Err(error);
        }
        // NB: The descriptor is new, and nothing else owns it.
        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        // NB: shm_unlink() reads a NUL-terminated name. fcntl() only changes the descriptor's flags.
        if unsafe { libc::shm_unlink(name.as_ptr()) } != 0
            || unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) } != 0
        {
            return Err(io::Error::last_os_error());
        }
        return Ok(fd);
    }
    Err(io::Error::from(io::ErrorKind::AlreadyExists))
}
