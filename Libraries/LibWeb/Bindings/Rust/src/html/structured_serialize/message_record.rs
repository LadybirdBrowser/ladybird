/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The record of a message that may reach agents in other processes, which Rust owns: the bytes of the values, the
//! SharedArrayBuffers that agents in this agent's realms share, the pin of the stores that the bytes name, and the
//! shared memory that hands the pixels of ImageBitmaps over.
//! C++ sees the record only when it is sent to or received from other processes.

use core::ffi::c_int;

use libjs_rust::api::{
    ArrayBuffer, DeferGc, Gc, OwnedSharedMemory, SharedStorePin, Trace, borrowed_from_raw_descriptor,
    duplicate_lifeline_from_raw_descriptor, lifeline_into_raw_descriptor,
};

pub(crate) struct SharedMemory {
    pub(crate) descriptor: OwnedSharedMemory,
    pub(crate) size: usize,
}

impl SharedMemory {
    /// A duplicate of a descriptor that C++ lends, unless it cannot be duplicated.
    ///
    /// # Safety
    ///
    /// The descriptor must stay open for the call.
    pub(crate) unsafe fn duplicate(descriptor: c_int, size: usize) -> Option<Self> {
        // SAFETY: The caller keeps the descriptor open.
        let descriptor = unsafe { borrowed_from_raw_descriptor(descriptor) }
            .try_clone_to_owned()
            .ok()?;
        Some(Self { descriptor, size })
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MessageRecord {
    #[gc(untraced)]
    pub(crate) bytes: Vec<u8>,
    /// The same-agent side table, by index from the bytes.
    pub(crate) shared_array_buffers: Vec<Gc<ArrayBuffer>>,
    /// The shared memory of ImageBitmap pixels, by index from the data holders.
    #[gc(untraced)]
    pub(crate) shared_memory: Vec<SharedMemory>,
    #[gc(untraced)]
    pub(crate) shared_store_pin: Option<SharedStorePin>,
}

impl MessageRecord {
    /// A record that another process sent, for the caller to allocate a cell with under the deferral. A lifeline that
    /// cannot be duplicated is left out, so that the stores it would keep may be gone, which deserializing throws for.
    ///
    /// # Safety
    ///
    /// Each descriptor must stay open for the call, and so must the one of the lifeline, if it is not negative.
    pub(crate) unsafe fn received(
        _: &DeferGc<'_>,
        bytes: &[u8],
        descriptors: &[c_int],
        sizes: &[usize],
        shared_store_lifeline: c_int,
    ) -> Self {
        let shared_memory = descriptors
            .iter()
            .zip(sizes)
            // SAFETY: The caller keeps the descriptors open.
            .map_while(|(&descriptor, &size)| unsafe { SharedMemory::duplicate(descriptor, size) })
            .collect();
        Self {
            bytes: bytes.to_vec(),
            shared_array_buffers: Vec::new(),
            shared_memory,
            // SAFETY: The caller keeps the descriptor open.
            shared_store_pin: (shared_store_lifeline >= 0)
                .then(|| unsafe { duplicate_lifeline_from_raw_descriptor(shared_store_lifeline) }.ok())
                .flatten()
                .map(SharedStorePin::Lifeline),
        }
    }

    /// The descriptor of a lifeline for the stores, which the caller owns, or a negative value for no lifeline, for
    /// sending the record to other processes.
    pub(crate) fn duplicate_shared_store_lifeline(&self) -> c_int {
        self.shared_store_pin
            .as_ref()
            .and_then(SharedStorePin::duplicate_lifeline)
            .map_or(-1, lifeline_into_raw_descriptor)
    }
}

impl Drop for MessageRecord {
    fn drop(&mut self) {
        if let Some(pin) = self.shared_store_pin.take() {
            pin.release();
        }
    }
}
