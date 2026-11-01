/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Uses C++ `TransportWindows` for named-pipe framing, shared payloads and handle-broker attachments.

#![allow(
    unsafe_code,
    reason = "the Windows transport and descriptor owners cross the C++ boundary"
)]

use std::ffi::c_void;
use std::io;
use std::ptr::NonNull;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::Attachment;
pub use crate::receive_queue::ReceivedMessage;

unsafe extern "C" {
    fn ipc_windows_transport_adopt(descriptor: i32) -> *mut c_void;
    fn ipc_windows_transport_take_over() -> *mut c_void;
    fn ipc_windows_transport_pair(local: *mut *mut c_void, remote: *mut i32) -> bool;
    fn ipc_windows_transport_destroy(transport: *mut c_void);
    fn ipc_windows_transport_close(transport: *mut c_void);
    fn ipc_windows_transport_is_open(transport: *const c_void) -> bool;
    fn ipc_windows_transport_send(
        transport: *mut c_void,
        bytes: *const u8,
        size: usize,
        descriptors: *const i32,
        descriptor_count: usize,
    ) -> bool;
    fn ipc_windows_transport_receive(transport: *mut c_void) -> *mut c_void;
    fn ipc_windows_message_bytes(message: *const c_void, size: *mut usize) -> *const u8;
    fn ipc_windows_message_attachment_count(message: *const c_void) -> usize;
    fn ipc_windows_message_take_attachment(message: *mut c_void) -> i32;
    fn ipc_windows_message_destroy(message: *mut c_void);
    fn ipc_windows_descriptor_clone(descriptor: i32) -> i32;
    fn ipc_windows_descriptor_close(descriptor: i32);
}

/// An owned LibCore descriptor. These are native handles or sockets, not Microsoft CRT file descriptors.
#[derive(Debug)]
pub struct OwnedDescriptor(i32);

impl OwnedDescriptor {
    /// # Safety
    /// The caller transfers sole ownership of a valid LibCore descriptor.
    pub unsafe fn from_raw(descriptor: i32) -> Self {
        Self(descriptor)
    }

    pub fn into_raw(self) -> i32 {
        let descriptor = self.0;
        std::mem::forget(self);
        descriptor
    }

    pub fn try_clone(&self) -> io::Result<Self> {
        // SAFETY: This owner keeps its descriptor live while C++ duplicates it.
        let descriptor = unsafe { ipc_windows_descriptor_clone(self.0) };
        if descriptor == -1 {
            return Err(io::Error::other("could not duplicate the IPC descriptor"));
        }
        Ok(Self(descriptor))
    }
}

impl Drop for OwnedDescriptor {
    fn drop(&mut self) {
        // SAFETY: This owner releases its descriptor exactly once, using the right close operation for sockets too.
        unsafe { ipc_windows_descriptor_close(self.0) };
    }
}

impl From<OwnedDescriptor> for std::fs::File {
    fn from(descriptor: OwnedDescriptor) -> Self {
        use std::os::windows::io::FromRawHandle;
        // SAFETY: LibCore descriptors contain the native handle value, and this conversion transfers its ownership.
        unsafe { Self::from_raw_handle(descriptor.into_raw() as isize as *mut c_void) }
    }
}

pub struct TransportWindows {
    native: NonNull<c_void>,
    reader: Mutex<()>,
    pending: AtomicUsize,
}

// SAFETY: C++ serializes sends, and `reader` serializes reads. Closing and dropping require exclusive access.
unsafe impl Send for TransportWindows {}
unsafe impl Sync for TransportWindows {}

impl TransportWindows {
    fn from_native(native: *mut c_void) -> io::Result<Self> {
        Ok(Self {
            native: NonNull::new(native).ok_or_else(|| io::Error::other("could not open the Windows IPC transport"))?,
            reader: Mutex::new(()),
            pending: AtomicUsize::new(0),
        })
    }

    pub fn new(descriptor: OwnedDescriptor) -> io::Result<Self> {
        // SAFETY: The native constructor consumes this descriptor on success and failure.
        Self::from_native(unsafe { ipc_windows_transport_adopt(descriptor.into_raw()) })
    }

    pub fn take_over() -> io::Result<Self> {
        // SAFETY: C++ takes the inherited descriptors and installs the process's handle-broker client.
        Self::from_native(unsafe { ipc_windows_transport_take_over() })
    }

    pub fn create_paired() -> io::Result<(Self, OwnedDescriptor)> {
        let mut native = std::ptr::null_mut();
        let mut descriptor = -1;
        // SAFETY: Both output slots are writable. On success they each own one end of a new pipe.
        if !unsafe { ipc_windows_transport_pair(&raw mut native, &raw mut descriptor) } {
            return Err(io::Error::other("could not create a Windows IPC pipe"));
        }
        Ok((Self::from_native(native)?, OwnedDescriptor(descriptor)))
    }

    pub fn post_message(&self, bytes: Vec<u8>, attachments: Vec<Attachment>) -> io::Result<()> {
        crate::validate_message_size(bytes.len(), attachments.len())?;
        let descriptors: Vec<_> = attachments
            .into_iter()
            .map(|attachment| attachment.into_raw())
            .collect();
        self.pending.fetch_add(1, Ordering::Relaxed);
        // SAFETY: The buffers remain live for this call. C++ consumes every attachment even if sending fails.
        let sent = unsafe {
            ipc_windows_transport_send(
                self.native.as_ptr(),
                bytes.as_ptr(),
                bytes.len(),
                descriptors.as_ptr(),
                descriptors.len(),
            )
        };
        self.pending.fetch_sub(1, Ordering::Relaxed);
        if sent {
            Ok(())
        } else {
            Err(io::ErrorKind::BrokenPipe.into())
        }
    }

    pub fn pending_message_count(&self) -> usize {
        self.pending.load(Ordering::Relaxed)
    }

    // Like the C++ Windows transport, sends complete before post_message returns.
    pub fn flush(&self) {}

    pub fn is_open(&self) -> bool {
        // SAFETY: The native transport stays live for this borrow; its open state is atomic.
        unsafe { ipc_windows_transport_is_open(self.native.as_ptr()) }
    }

    pub fn receive(&self) -> Option<ReceivedMessage> {
        let _reader = self.reader.lock().unwrap_or_else(|error| error.into_inner());
        // SAFETY: This is the only reader. The returned message owns its bytes and attachments until dropped.
        let message = NonNull::new(unsafe { ipc_windows_transport_receive(self.native.as_ptr()) })?;
        struct Message(NonNull<c_void>);
        impl Drop for Message {
            fn drop(&mut self) {
                // SAFETY: This guard uniquely owns the native message.
                unsafe { ipc_windows_message_destroy(self.0.as_ptr()) };
            }
        }
        let message = Message(message);
        let mut size = 0;
        // SAFETY: The message is live and `size` is writable. An empty message may have a null data pointer.
        let data = unsafe { ipc_windows_message_bytes(message.0.as_ptr(), &raw mut size) };
        let bytes = if size == 0 {
            Vec::new()
        } else {
            // SAFETY: The native message owns exactly `size` readable bytes for the duration of this copy.
            #[expect(clippy::disallowed_methods, reason = "the empty message case is handled above")]
            let bytes = unsafe { std::slice::from_raw_parts(data, size) };
            bytes.to_vec()
        };
        // SAFETY: The message is live and we take each attachment exactly once.
        let count = unsafe { ipc_windows_message_attachment_count(message.0.as_ptr()) };
        let attachments = (0..count)
            .map(|_| unsafe {
                Attachment::from_descriptor(OwnedDescriptor::from_raw(ipc_windows_message_take_attachment(
                    message.0.as_ptr(),
                )))
            })
            .collect();
        Some(ReceivedMessage { bytes, attachments })
    }

    pub fn close_after_sending_all_pending_messages(&mut self) {
        self.close();
    }

    pub fn close(&mut self) {
        // SAFETY: Exclusive access means no send or receive is in flight.
        unsafe { ipc_windows_transport_close(self.native.as_ptr()) };
    }
}

impl Drop for TransportWindows {
    fn drop(&mut self) {
        // SAFETY: This uniquely owns the native allocation, and no borrows remain.
        unsafe { ipc_windows_transport_destroy(self.native.as_ptr()) };
    }
}
