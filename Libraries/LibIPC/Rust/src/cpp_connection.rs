/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What Rust and a connection whose transport C++ owns hand each other.

#![allow(unsafe_code)]

use core::ffi::c_int;

use crate::attachment::Attachment;

/// A Mach port name and its disposition on Apple, and a descriptor elsewhere: `IPC::RawAttachment`.
#[repr(C)]
pub struct RawAttachment {
    value: c_int,
    disposition: u32,
}

impl RawAttachment {
    /// # Safety
    ///
    /// The raw attachment must hand over one owned descriptor or Mach right.
    unsafe fn into_attachment(self) -> Attachment {
        #[cfg(target_vendor = "apple")]
        {
            use crate::mach::{
                MACH_MSG_TYPE_MAKE_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE, MACH_MSG_TYPE_MOVE_RECEIVE,
                MACH_MSG_TYPE_MOVE_SEND_ONCE, MachPort, PortRight,
            };
            let right = match self.disposition {
                MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MAKE_SEND | MACH_MSG_TYPE_MAKE_SEND_ONCE => {
                    PortRight::Receive
                }
                MACH_MSG_TYPE_MOVE_SEND_ONCE => PortRight::SendOnce,
                _ => PortRight::Send,
            };
            Attachment::from_mach_port(MachPort::adopt(self.value as u32, right), self.disposition)
        }
        #[cfg(all(unix, not(target_vendor = "apple")))]
        {
            use std::os::fd::FromRawFd;
            // SAFETY: The caller hands over the descriptor.
            Attachment::from(unsafe { std::os::fd::OwnedFd::from_raw_fd(self.value) })
        }
        #[cfg(windows)]
        {
            // SAFETY: The caller hands over the descriptor.
            Attachment::from_descriptor(unsafe { crate::transport_windows::OwnedDescriptor::from_raw(self.value) })
        }
    }

    /// # Safety
    ///
    /// Each of the raw attachments must hand over its descriptor or Mach right.
    pub unsafe fn take_all(raw: &[Self]) -> Vec<Attachment> {
        raw.iter()
            .map(|attachment| {
                let owned = Self {
                    value: attachment.value,
                    disposition: attachment.disposition,
                };
                // SAFETY: The caller hands over what each raw attachment names.
                unsafe { owned.into_attachment() }
            })
            .collect()
    }

    pub fn from_attachment(attachment: Attachment) -> Self {
        #[cfg(target_vendor = "apple")]
        {
            let (port, disposition) = attachment.into_mach_port();
            Self {
                value: core::mem::ManuallyDrop::new(port).name() as c_int,
                disposition,
            }
        }
        #[cfg(all(unix, not(target_vendor = "apple")))]
        {
            use std::os::fd::IntoRawFd;
            Self {
                value: attachment
                    .into_fd()
                    .expect("a Unix attachment is a descriptor")
                    .into_raw_fd(),
                disposition: 0,
            }
        }
        #[cfg(windows)]
        {
            Self {
                value: attachment.into_descriptor().into_raw(),
                disposition: 0,
            }
        }
    }
}
