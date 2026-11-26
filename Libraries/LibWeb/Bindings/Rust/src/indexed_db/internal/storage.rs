/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The databases of the agent, by their storage key and name, and the connection queues of those,
//! https://w3c.github.io/IndexedDB/#connection-queues. They live as long as the thread of the agent.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Vm};

use crate::indexed_db::remote::Remote;

unsafe extern "C" {
    fn gc_root_create(cell: *mut c_void) -> *mut c_void;
    fn gc_root_destroy(root: *mut c_void);
}

#[derive(Trace)]
pub(crate) struct Storage {
    remote: Remote,
}

static STORAGE_CLASS: EmbedderClass<Storage> = EmbedderClass::new("IndexedDBStorage");

impl EmbedderValue for Storage {
    const CLASS: &'static EmbedderClass<Self> = &STORAGE_CLASS;
}

pub(crate) type StorageRef = Gc<EmbedderCell<Storage>>;

thread_local! {
    // NB: Keeping the root here lets the leak checker see it as reachable.
    static STORAGE: Cell<Option<(StorageRef, NonNull<c_void>)>> = const { Cell::new(None) };
}

/// The storage of the current thread's agent, created on first use.
pub(crate) fn storage(vm: &Vm) -> StorageRef {
    if let Some((storage, _)) = STORAGE.get() {
        return storage;
    }
    let storage = STORAGE_CLASS.allocate(vm, |_| Storage {
        remote: Remote::default(),
    });
    // NB: The storage lives as long as the agent of the thread, which roots its cell.
    // SAFETY: The cell is a live cell of the heap.
    let root = NonNull::new(unsafe { gc_root_create(storage.as_ptr().cast()) }).expect("a root is allocated");
    STORAGE.set(Some((storage, root)));
    storage
}

/// Releases the storage of the current thread's agent, which goes away with its heap.
pub(crate) fn shut_down_storage() {
    if let Some((_, root)) = STORAGE.take() {
        // SAFETY: The root is the one storage() created, and the heap it is in is alive.
        unsafe { gc_root_destroy(root.as_ptr()) };
    }
}

impl Storage {
    pub(crate) fn remote(&self) -> &Remote {
        &self.remote
    }
}
