/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a message carries besides its bytes: a Mach port right on Apple platforms, a handle on Windows, and a file
//! descriptor elsewhere.

#[cfg(unix)]
use std::os::fd::OwnedFd;
#[cfg(windows)]
use std::os::windows::io::OwnedHandle;

#[cfg(target_vendor = "apple")]
use crate::mach::{self, MachPort};

#[cfg(target_vendor = "apple")]
#[derive(Debug)]
pub struct Attachment {
    port: MachPort,
    disposition: u32,
}

#[cfg(target_vendor = "apple")]
impl Attachment {
    /// An attachment moving a right in a message, with the disposition the right is moved with.
    pub fn from_mach_port(port: MachPort, disposition: u32) -> Self {
        Self { port, disposition }
    }

    pub fn from_fd(fd: OwnedFd) -> std::io::Result<Self> {
        Ok(Self::from_mach_port(
            mach::file_port_from_fd(fd)?,
            mach::MACH_MSG_TYPE_MOVE_SEND,
        ))
    }

    pub fn into_fd(self) -> std::io::Result<OwnedFd> {
        mach::fd_from_file_port(self.port)
    }

    pub fn into_mach_port(self) -> (MachPort, u32) {
        (self.port, self.disposition)
    }
}

#[cfg(all(unix, not(target_vendor = "apple")))]
#[derive(Debug)]
pub struct Attachment {
    fd: OwnedFd,
}

#[cfg(all(unix, not(target_vendor = "apple")))]
impl Attachment {
    pub fn from_fd(fd: OwnedFd) -> std::io::Result<Self> {
        Ok(Self { fd })
    }

    pub fn into_fd(self) -> std::io::Result<OwnedFd> {
        Ok(self.fd)
    }

    pub fn as_fd(&self) -> std::os::fd::BorrowedFd<'_> {
        std::os::fd::AsFd::as_fd(&self.fd)
    }
}

#[cfg(all(unix, not(target_vendor = "apple")))]
impl From<OwnedFd> for Attachment {
    fn from(fd: OwnedFd) -> Self {
        Self { fd }
    }
}

#[cfg(windows)]
#[derive(Debug)]
pub struct Attachment {
    handle: OwnedHandle,
}

#[cfg(windows)]
impl Attachment {
    pub fn from_handle(handle: OwnedHandle) -> Self {
        Self { handle }
    }

    pub fn into_handle(self) -> OwnedHandle {
        self.handle
    }
}
