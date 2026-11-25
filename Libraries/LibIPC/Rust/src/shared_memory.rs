/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The anonymous files that carry the payloads of socket messages too large for a frame.
//!
//! On Linux the sender seals the file once it has written the payload, and the receiver maps it rather than copying it:
//! a sealed file can't change, shrink or grow, so the mapping holds what the sender wrote, and reading it can't fault.

#![allow(unsafe_code)]

use std::io;
use std::os::fd::{FromRawFd, OwnedFd};

#[cfg(target_os = "linux")]
pub(crate) fn create_anonymous_file() -> io::Result<OwnedFd> {
    // NB: memfd_create() reads a NUL-terminated name and returns a new descriptor.
    let fd = unsafe { libc::memfd_create(c"".as_ptr(), libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    // NB: The descriptor is new, and nothing else owns it.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

#[cfg(target_os = "linux")]
const SEALS_OF_A_PAYLOAD: libc::c_int = libc::F_SEAL_WRITE | libc::F_SEAL_SHRINK | libc::F_SEAL_GROW;

/// Keeps anything from changing the file from now on, so that a receiver may map it.
#[cfg(target_os = "linux")]
pub(crate) fn seal(file: &std::fs::File) -> io::Result<()> {
    use std::os::fd::AsRawFd;
    // NB: F_ADD_SEALS only changes the seals of the owned file.
    if unsafe {
        libc::fcntl(
            file.as_raw_fd(),
            libc::F_ADD_SEALS,
            SEALS_OF_A_PAYLOAD | libc::F_SEAL_SEAL,
        )
    } < 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

/// A read-only mapping of the start of a sealed file, which the mapping keeps alive.
#[cfg(target_os = "linux")]
pub struct MappedPayload {
    address: std::ptr::NonNull<u8>,
    size: usize,
}

// SAFETY: The mapping is read-only, and the file under it can't change.
#[cfg(target_os = "linux")]
unsafe impl Send for MappedPayload {}
// SAFETY: As above.
#[cfg(target_os = "linux")]
unsafe impl Sync for MappedPayload {}

#[cfg(target_os = "linux")]
impl MappedPayload {
    /// Maps the first `size` bytes of a file that its sender sealed, and refuses one that is unsealed or too small.
    pub(crate) fn map(file: OwnedFd, size: usize) -> io::Result<Self> {
        use std::os::fd::AsRawFd;
        if size == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "empty payload file"));
        }
        // NB: F_GET_SEALS only reads the seals of the owned file.
        let seals = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GET_SEALS) };
        if seals < 0 {
            return Err(io::Error::last_os_error());
        }
        if seals & SEALS_OF_A_PAYLOAD != SEALS_OF_A_PAYLOAD {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "unsealed payload file"));
        }
        // NB: fstat() rather than the standard library's metadata(), which tries statx() first, and the sandboxes of
        //     the helper processes allow only the former.
        let mut status: libc::stat = unsafe { std::mem::zeroed() };
        // NB: The call writes the status of the owned file to the local.
        if unsafe { libc::fstat(file.as_raw_fd(), &raw mut status) } < 0 {
            return Err(io::Error::last_os_error());
        }
        // NB: The mapping would keep the rest of a longer file alive for as long as the message waits.
        if status.st_size as u64 != size as u64 {
            return Err(io::Error::from(io::ErrorKind::InvalidData));
        }
        // NB: A file of larger pages than the system's, as one in hugetlbfs, is mapped in those pages, and the size of
        //     the payload would not unmap them.
        // NB: sysconf() only reads a system value.
        if status.st_blksize as libc::c_long != unsafe { libc::sysconf(libc::_SC_PAGESIZE) } {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "payload file of unusual pages",
            ));
        }
        // NB: Reading a hole through the mapping has to allocate a page, and fails with SIGBUS if it can't. The sender
        //     writes the whole payload, so a file with holes is refused.
        // NB: lseek() only reads where the owned file's first hole is.
        let first_hole = unsafe { libc::lseek(file.as_raw_fd(), 0, libc::SEEK_HOLE) };
        if first_hole < 0 {
            return Err(io::Error::last_os_error());
        }
        if (first_hole as u64) < size as u64 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "payload file with holes"));
        }
        // NB: The call maps the owned file read-only, and the mapping keeps the file once the descriptor is closed.
        let address = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ,
                libc::MAP_SHARED,
                file.as_raw_fd(),
                0,
            )
        };
        if address == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        Ok(Self {
            address: std::ptr::NonNull::new(address.cast()).expect("a successful mmap() is not null"),
            size,
        })
    }
}

#[cfg(target_os = "linux")]
impl AsRef<[u8]> for MappedPayload {
    fn as_ref(&self) -> &[u8] {
        // SAFETY: The mapping has size readable bytes until this is dropped, and its file can't change or shrink.
        unsafe { ak::slice_from_raw_parts(self.address.as_ptr(), self.size) }
    }
}

#[cfg(target_os = "linux")]
impl Drop for MappedPayload {
    fn drop(&mut self) {
        // NB: This owns the mapping, which nothing reads afterwards.
        unsafe { libc::munmap(self.address.as_ptr().cast(), self.size) };
    }
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

/// A payload file as a peer that does not seal it would send it.
#[cfg(all(test, target_os = "linux"))]
pub(crate) fn create_unsealed_file_holding(bytes: &[u8]) -> std::fs::File {
    use std::io::Write;
    let mut file = std::fs::File::from(create_anonymous_file().unwrap());
    file.write_all(bytes).unwrap();
    file
}
