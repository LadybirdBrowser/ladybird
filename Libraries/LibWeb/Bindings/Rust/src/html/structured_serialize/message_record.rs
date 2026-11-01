/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The record of a message that may reach agents in other processes, which Rust owns: the bytes of the values, the
//! SharedArrayBuffers that agents in this process share, and the shared memory that agents in other processes map.
//! C++ sees the record only when it is sent to or received from other processes.

use core::ffi::c_int;

use libjs_rust::api::{
    ArrayBuffer, AsSharedMemory, DeferGc, Gc, OwnedSharedMemory, Trace, borrowed_from_raw_descriptor,
    into_raw_descriptor,
};

pub(crate) struct SharedMemory {
    pub(crate) descriptor: OwnedSharedMemory,
    pub(crate) size: usize,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MessageRecord {
    #[gc(untraced)]
    pub(crate) bytes: Vec<u8>,
    /// The same-process side table, by index from the bytes.
    pub(crate) shared_array_buffers: Vec<Gc<ArrayBuffer>>,
    /// The shared memory of the SharedArrayBuffers, by index from the bytes.
    #[gc(untraced)]
    pub(crate) shared_memory: Vec<SharedMemory>,
}

impl MessageRecord {
    /// A record that another process sent, for the caller to allocate a cell with under the deferral. A descriptor that
    /// cannot be duplicated leaves out the shared memory from it on, which deserializing then throws for.
    ///
    /// # Safety
    ///
    /// Each descriptor must stay open for the call.
    pub(crate) unsafe fn received(_: &DeferGc<'_>, bytes: &[u8], descriptors: &[c_int], sizes: &[usize]) -> Self {
        let shared_memory = descriptors
            .iter()
            .zip(sizes)
            .map_while(|(&descriptor, &size)| {
                // SAFETY: The caller keeps the descriptor open.
                let descriptor = unsafe { borrowed_from_raw_descriptor(descriptor) }
                    .try_clone_to_owned()
                    .ok()?;
                Some(SharedMemory { descriptor, size })
            })
            .collect();
        Self {
            bytes: bytes.to_vec(),
            shared_array_buffers: Vec::new(),
            shared_memory,
        }
    }

    /// Duplicates of the descriptors of the shared memory, which the caller owns, and their sizes, for sending the
    /// record to other processes.
    pub(crate) fn duplicate_shared_memory(&self) -> (Vec<c_int>, Vec<usize>) {
        self.shared_memory
            .iter()
            .map_while(|shared_memory| {
                let descriptor = shared_memory.descriptor.as_shared_memory().try_clone_to_owned().ok()?;
                Some((into_raw_descriptor(descriptor), shared_memory.size))
            })
            .unzip()
    }
}
