/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the agent of a thread keeps for its entries in the lock table: the objects of its lock requests and of its
//! locks, by the id of the entry. It acts on the notices that the table leaves for it from its own lock task queue, so
//! these objects never leave the thread.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Trace, Vm};

use crate::bindings::Exception;
use crate::bindings::interface::with_temporary_execution_context_and_callbacks;
use crate::bindings::promise::reject_promise_with_exception;
use crate::html::message_port::EventLoopReference;
use crate::web_locks::lock::LockDataRef;
use crate::web_locks::lock_manager::LockManager;
use crate::web_locks::lock_request::LockRequestRef;
use crate::web_locks::lock_table::{self, AgentId, EntryId, Notice};

unsafe extern "C" {
    fn gc_root_create(cell: *mut c_void) -> *mut c_void;
    fn gc_root_destroy(root: *mut c_void);
    fn web_locks_queue_notices_task(steps: unsafe extern "C" fn(*mut c_void));
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct AgentObjects {
    requests: GcRefCell<HashMap<EntryId, LockRequestRef>>,
    locks: GcRefCell<HashMap<EntryId, LockDataRef>>,
}

static AGENT_OBJECTS_CLASS: EmbedderClass<AgentObjects> = EmbedderClass::new("WebLocksAgentObjects");

impl EmbedderValue for AgentObjects {
    const CLASS: &'static EmbedderClass<Self> = &AGENT_OBJECTS_CLASS;
}

type AgentObjectsRef = Gc<EmbedderCell<AgentObjects>>;

thread_local! {
    /// The id under which the lock table knows the agent of this thread, from the first time the agent needs one.
    static AGENT_ID: Cell<Option<AgentId>> = const { Cell::new(None) };

    // NB: Keeping the root here lets the leak checker see it as reachable.
    static AGENT_OBJECTS: Cell<Option<(AgentObjectsRef, NonNull<c_void>)>> = const { Cell::new(None) };
}

/// Gives out the ids of agents and of entries, which are unique in the process.
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

pub(crate) fn next_entry_id() -> EntryId {
    NEXT_ID.fetch_add(1, Ordering::Relaxed)
}

/// The id under which the lock table knows the agent of this thread. The first call adds the agent to the table.
pub(crate) fn agent_id() -> AgentId {
    if let Some(id) = AGENT_ID.get() {
        return id;
    }
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    lock_table::add_agent(id, EventLoopReference::current());
    AGENT_ID.set(Some(id));
    id
}

fn agent_objects(vm: &Vm) -> AgentObjectsRef {
    if let Some((objects, _)) = AGENT_OBJECTS.get() {
        return objects;
    }
    let objects = AGENT_OBJECTS_CLASS.allocate(vm, |_| AgentObjects {
        requests: GcRefCell::new(HashMap::new()),
        locks: GcRefCell::new(HashMap::new()),
    });
    // NB: The objects live as long as the agent of the thread, which roots their cell.
    // SAFETY: The cell is a live cell of the heap.
    let root = NonNull::new(unsafe { gc_root_create(objects.as_ptr().cast()) }).expect("a root is allocated");
    AGENT_OBJECTS.set(Some((objects, root)));
    objects
}

fn existing_agent_objects() -> Option<AgentObjectsRef> {
    AGENT_OBJECTS.get().map(|(objects, _)| objects)
}

pub(crate) fn keep_request(vm: &Vm, id: EntryId, request: LockRequestRef) {
    agent_objects(vm).requests.borrow_mut(vm).insert(id, request);
}

pub(crate) fn take_request(vm: &Vm, id: EntryId) -> Option<LockRequestRef> {
    existing_agent_objects()?.requests.borrow_mut(vm).remove(&id)
}

pub(crate) fn keep_lock(vm: &Vm, id: EntryId, lock: LockDataRef) {
    agent_objects(vm).locks.borrow_mut(vm).insert(id, lock);
}

pub(crate) fn take_lock(vm: &Vm, id: EntryId) -> Option<LockDataRef> {
    existing_agent_objects()?.locks.borrow_mut(vm).remove(&id)
}

/// Acts on the notices that the lock table left for the agent of this thread. This runs on the lock task queue.
pub(crate) fn act_on_notices(vm: &Vm) {
    let Some(agent) = AGENT_ID.get() else {
        return;
    };
    loop {
        let notices = lock_table::take_notices(agent);
        if notices.is_empty() {
            return;
        }
        for notice in notices {
            match notice {
                Notice::Grant(id) => {
                    let Some(request) = take_request(vm, id) else {
                        // NB: The agent let go of the request, as its client went away. So nothing takes the lock.
                        lock_table::release_lock_with_id(id, agent);
                        continue;
                    };
                    with_temporary_execution_context_and_callbacks(request.realm, || {
                        LockManager::grant_request(vm, request);
                    });
                }
                Notice::Steal(id) => {
                    let Some(lock) = take_lock(vm, id) else {
                        continue;
                    };
                    // https://w3c.github.io/web-locks/#request-a-lock, step 3.4.1.1.2:
                    // 2. Reject lock’s released promise with an "AbortError" DOMException.
                    with_temporary_execution_context_and_callbacks(lock.realm, || {
                        reject_promise_with_exception(
                            vm,
                            lock.released_promise,
                            Exception::Dom("AbortError", Utf16String::from_utf8("Lock was stolen")),
                        );
                    });
                }
            }
        }
    }
}

/// Runs on the thread of an agent that the lock table left notices for, from its event loop.
pub(crate) unsafe extern "C" fn wake_agent(_: AgentId) {
    unsafe extern "C" fn run(vm: *mut c_void) {
        // SAFETY: LibWeb passes the VM of the thread.
        act_on_notices(unsafe { &*vm.cast::<Vm>() });
    }
    // SAFETY: The steps are a plain function.
    unsafe { web_locks_queue_notices_task(run) };
}

/// Terminates the remaining locks and requests of the client, which is an environment of the agent of this thread.
/// This runs on the lock task queue.
pub(crate) fn terminate_remaining_locks_and_requests_of_client(vm: &Vm, client_id: &Utf16String) {
    let Some(agent) = AGENT_ID.get() else {
        return;
    };
    lock_table::terminate_remaining_locks_and_requests_of_client(agent, client_id);

    // NB: The objects go before the agent acts on its notices, so a grant to the client finds no request, and releases
    //     the lock that the table made for it.
    if let Some(objects) = existing_agent_objects() {
        objects
            .requests
            .borrow_mut(vm)
            .retain(|_, request| request.client_id != *client_id);
        objects
            .locks
            .borrow_mut(vm)
            .retain(|_, lock| lock.client_id != *client_id);
    }
    act_on_notices(vm);
}

/// Whether the agent of this thread ever used the lock table.
pub(crate) fn has_agent_id() -> bool {
    AGENT_ID.get().is_some()
}

/// Lets go of what the agent of this thread has in the lock table, and of the objects that it keeps for that, as the
/// agent goes away with its heap.
pub(crate) fn shut_down() {
    // NB: The thread can host another agent after this one, which then gets an id of its own.
    if let Some(agent) = AGENT_ID.take() {
        lock_table::terminate_remaining_locks_and_requests_of_agent(agent);
    }
    if let Some((_, root)) = AGENT_OBJECTS.take() {
        // SAFETY: The root is the one agent_objects() created, and the heap it is in is alive.
        unsafe { gc_root_destroy(root.as_ptr()) };
    }
}
