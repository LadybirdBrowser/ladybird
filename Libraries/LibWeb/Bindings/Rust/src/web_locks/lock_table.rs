/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The lock managers of this process, https://w3c.github.io/web-locks/#lock-manager. There is one for each storage key,
//! and all agents of the key share it, the agents of worker threads too.
//!
//! The table holds plain data only. A lock request or a lock is an entry with an id, and the agent of the entry keeps
//! the promises, the callback and the signal under that id (see agent.rs). When the table grants a request or takes a
//! lock away, it leaves a notice for the agent of the entry, which then does the rest on its own thread. Nothing calls
//! into script or allocates on a heap while the table is locked.
//!
//! The names and client ids are AK strings. Their storage counts its references atomically, so any thread can drop
//! them.

use core::hash::BuildHasherDefault;
use std::collections::hash_map::DefaultHasher;
use std::collections::{HashMap, VecDeque};
use std::sync::{LazyLock, Mutex, MutexGuard, PoisonError};

use ak::Utf16String;
use liburl_rust::origin::{Host, Origin};

use crate::html::message_port::EventLoopReference;
use crate::storage_api::storage_key::StorageKey;
use crate::web_locks::agent::wake_agent;
use crate::web_locks::lock_manager::{LockInfo, LockMode};

/// An agent, as the table knows it.
pub(crate) type AgentId = u64;

/// The id of a lock request, which the lock that it turns into keeps.
pub(crate) type EntryId = u64;

/// The storage key of a lock manager, as a key of the table.
#[derive(Clone, PartialEq, Eq, Hash)]
pub(crate) struct LockManagerKey {
    scheme: String,
    host: Host,
    port: Option<u16>,
}

impl LockManagerKey {
    pub(crate) fn new(storage_key: StorageKey) -> Self {
        // NB: Storage keys are equal when their origins are same origin, which compares these.
        match storage_key.origin {
            Origin::Tuple { scheme, host, port, .. } => Self { scheme, host, port },
            Origin::Opaque { .. } => unreachable!("a storage key never has an opaque origin"),
        }
    }
}

/// The plain data of a lock request or of a lock: https://w3c.github.io/web-locks/#lock-request and
/// https://w3c.github.io/web-locks/#lock.
pub(crate) struct Entry {
    pub(crate) id: EntryId,
    pub(crate) agent: AgentId,
    pub(crate) client_id: Utf16String,
    pub(crate) name: Utf16String,
    pub(crate) mode: LockMode,
}

impl Entry {
    fn info(&self) -> LockInfo {
        LockInfo {
            name: Some(self.name.clone()),
            mode: Some(self.mode),
            client_id: Some(self.client_id.clone()),
        }
    }
}

/// What the agent of an entry has to do with the objects that it keeps for the entry.
#[derive(Clone, Copy)]
pub(crate) enum Notice {
    /// The table granted the request, and moved its entry to the held lock set.
    Grant(EntryId),
    /// The table removed the lock from the held lock set, as another request stole it.
    Steal(EntryId),
}

/// What became of a lock request.
pub(crate) enum RequestOutcome {
    Enqueued,
    NotGrantable,
    /// The agent of the request ended, so the table does not take its requests any more.
    AgentEnded,
}

// NB: A fixed hasher keeps the order of query()'s pending list the same from run to run.
type LockRequestQueueMap = HashMap<Utf16String, VecDeque<Entry>, BuildHasherDefault<DefaultHasher>>;

// https://w3c.github.io/web-locks/#lock-manager
#[derive(Default)]
struct LockManager {
    // https://w3c.github.io/web-locks/#lock-request-queue-map
    // NB: A queue that becomes empty leaves the map, as nothing can tell an empty queue from one that is not there.
    lock_request_queue_map: LockRequestQueueMap,
    // https://w3c.github.io/web-locks/#held-lock-set
    held_lock_set: Vec<Entry>,
}

impl LockManager {
    fn is_empty(&self) -> bool {
        self.lock_request_queue_map.is_empty() && self.held_lock_set.is_empty()
    }

    // https://w3c.github.io/web-locks/#grantable
    fn is_grantable(&self, request: &Entry) -> bool {
        // 1. Let manager be request’s manager.
        // 2. Let queueMap be manager’s lock request queue map.
        // 3. Let name be request’s name.
        // 4. Let queue be the result of getting the lock request queue from queueMap for name.
        let queue = self.lock_request_queue_map.get(&request.name);

        // 5. Let held be manager’s held lock set
        let held = &self.held_lock_set;

        // 6. Let mode be request’s mode
        let mode = request.mode;

        // 7. If queue is not empty and request is not the first item in queue, then return false.
        if queue
            .and_then(VecDeque::front)
            .is_some_and(|first| first.id != request.id)
        {
            return false;
        }

        // 8. If mode is "exclusive", then return true if no lock in held has name equal to name, and false otherwise.
        if mode == LockMode::Exclusive {
            return !held.iter().any(|lock| lock.name == request.name);
        }

        // 9. Otherwise, mode is "shared"; return true if no lock in held has mode "exclusive" and has name equal to
        //    name, and false otherwise.
        !held
            .iter()
            .any(|lock| lock.mode == LockMode::Exclusive && lock.name == request.name)
    }
}

/// An agent that the table takes requests from, and leaves notices for.
struct Agent {
    event_loop: EventLoopReference,
    notices: Vec<Notice>,
    is_wake_pending: bool,
}

#[derive(Default)]
struct Table {
    agents: HashMap<AgentId, Agent>,
    managers: HashMap<LockManagerKey, LockManager>,
}

static TABLE: LazyLock<Mutex<Table>> = LazyLock::new(Mutex::default);

fn lock() -> MutexGuard<'static, Table> {
    TABLE.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Table {
    /// Leaves the notice for the agent, and wakes the agent to act on it unless `caller` is that agent. A caller acts on
    /// its own notices once it has unlocked the table.
    fn notify(&mut self, agent: AgentId, notice: Notice, caller: Option<AgentId>) {
        let Some(record) = self.agents.get_mut(&agent) else {
            return;
        };
        record.notices.push(notice);
        if caller == Some(agent) || record.is_wake_pending {
            return;
        }
        record.is_wake_pending = true;
        // NB: The post only queues the wake on the event loop of the agent, which never locks the table to accept it.
        record.event_loop.post(wake_agent, agent);
    }

    // https://w3c.github.io/web-locks/#process-the-lock-request-queue
    fn process_lock_request_queue(&mut self, key: &LockManagerKey, name: &Utf16String, caller: Option<AgentId>) {
        let Some(manager) = self.managers.get_mut(key) else {
            return;
        };

        // 1. Assert: these steps are running on the lock task queue.

        // 2. For each request of queue:
        let mut granted = Vec::new();
        while let Some(request) = manager.lock_request_queue_map.get(name).and_then(VecDeque::front) {
            // 1. If request is not grantable, then return.
            // NOTE: Only the first item in a queue is grantable. Therefore, if something is not grantable then all the
            //       following items are automatically not grantable.
            if !manager.is_grantable(request) {
                break;
            }

            // 2. Remove request from queue.
            let request = manager
                .lock_request_queue_map
                .get_mut(name)
                .and_then(VecDeque::pop_front)
                .expect("the queue has the request");

            // 3. to 12. and 14.
            // NB: The agent of the request does these steps as it acts on the notice, since they make objects on its
            //     heap. Its lock keeps the id of the request.

            // 13. Append lock to manager’s held lock set.
            granted.push((request.agent, request.id));
            manager.held_lock_set.push(request);
        }
        if manager.lock_request_queue_map.get(name).is_some_and(VecDeque::is_empty) {
            manager.lock_request_queue_map.remove(name);
        }
        if manager.is_empty() {
            self.managers.remove(key);
        }
        for (agent, id) in granted {
            self.notify(agent, Notice::Grant(id), caller);
        }
    }

    /// Steps 1 and 2 of terminating remaining locks and requests, for the entries that match, given the agent that runs
    /// the steps if it acts on its own notices.
    fn terminate_remaining_locks_and_requests(&mut self, matches: impl Fn(&Entry) -> bool, caller: Option<AgentId>) {
        let mut affected_queues = Vec::new();
        for (key, manager) in &mut self.managers {
            // 1. For each lock request request with agent equal to agent:
            //    1. Abort the request request.
            for (name, queue) in &mut manager.lock_request_queue_map {
                let length = queue.len();
                queue.retain(|request| !matches(request));
                if queue.len() != length {
                    affected_queues.push((key.clone(), name.clone()));
                }
            }

            // 2. For each lock lock with agent equal to agent:
            //    1. Release the lock lock.
            manager.held_lock_set.retain(|lock| {
                if !matches(lock) {
                    return true;
                }
                affected_queues.push((key.clone(), lock.name.clone()));
                false
            });
        }

        // NB: Each abort and each release processes the queue of its name. The queues are processed once all entries
        //     are gone, which grants the same requests.
        for (key, name) in affected_queues {
            self.process_lock_request_queue(&key, &name, caller);
        }
        self.managers.retain(|_, manager| !manager.is_empty());
    }
}

/// Lets the table take requests from the agent and leave notices for it, which wake its event loop.
pub(crate) fn add_agent(agent: AgentId, event_loop: EventLoopReference) {
    lock().agents.insert(
        agent,
        Agent {
            event_loop,
            notices: Vec::new(),
            is_wake_pending: false,
        },
    );
}

/// The notices that the table left for the agent, in the order it left them.
pub(crate) fn take_notices(agent: AgentId) -> Vec<Notice> {
    let mut table = lock();
    let Some(record) = table.agents.get_mut(&agent) else {
        return Vec::new();
    };
    record.is_wake_pending = false;
    core::mem::take(&mut record.notices)
}

/// Steps 3.1 to 3.6 of requesting a lock, https://w3c.github.io/web-locks/#request-a-lock, by the agent of the request.
pub(crate) fn request_lock(key: &LockManagerKey, request: Entry, if_available: bool, steal: bool) -> RequestOutcome {
    let mut table = lock();
    let agent = request.agent;
    if !table.agents.contains_key(&agent) {
        return RequestOutcome::AgentEnded;
    }
    let name = request.name.clone();

    // 1. Let queueMap be manager’s lock request queue map.
    // 2. Let queue be the result of getting the lock request queue from queueMap for name.
    // 3. Let held be manager’s held lock set.
    let manager = table.managers.entry(key.clone()).or_default();

    // 4. If steal is true, then run these steps:
    if steal {
        // 1. For each lock of held:
        //    1. If lock’s name is name, then run these steps:
        //       1. Remove lock from held.
        //       2. Reject lock’s released promise with an "AbortError" DOMException.
        //          NB: The agent of the lock does this as it acts on the notice.
        let mut stolen = Vec::new();
        manager.held_lock_set.retain(|lock| {
            if lock.name != name {
                return true;
            }
            stolen.push((lock.agent, lock.id));
            false
        });

        // 2. Prepend request in queue.
        manager
            .lock_request_queue_map
            .entry(name.clone())
            .or_default()
            .push_front(request);

        for (lock_agent, id) in stolen {
            table.notify(lock_agent, Notice::Steal(id), Some(agent));
        }
    }
    // 5. Otherwise, run these steps:
    else {
        // 1. If ifAvailable is true and request is not grantable, then enqueue the following steps on callback’s
        //    relevant settings object’s responsible event loop:
        //    NB: The agent of the request does this, as the caller is that agent.
        if if_available && !manager.is_grantable(&request) {
            if manager.is_empty() {
                table.managers.remove(key);
            }
            return RequestOutcome::NotGrantable;
        }

        // 2. Enqueue request in queue.
        manager
            .lock_request_queue_map
            .entry(name.clone())
            .or_default()
            .push_back(request);
    }

    // 6. Process the lock request queue queue.
    table.process_lock_request_queue(key, &name, Some(agent));
    RequestOutcome::Enqueued
}

/// Steps 2 to 7 of releasing the lock, https://w3c.github.io/web-locks/#release-the-lock, by the agent of the lock.
pub(crate) fn release_lock(key: &LockManagerKey, name: &Utf16String, id: EntryId, agent: AgentId) {
    let mut table = lock();

    // 2. Let manager be lock’s manager.
    // 3. Let queueMap be manager’s lock request queue map.
    // 4. Let name be lock’s resource name.
    // 5. Let queue be the result of getting the lock request queue from queueMap for name.
    // 6. Remove lock from the manager’s held lock set.
    let Some(manager) = table.managers.get_mut(key) else {
        return;
    };
    manager.held_lock_set.retain(|lock| lock.id != id);

    // 7. Process the lock request queue queue.
    table.process_lock_request_queue(key, name, Some(agent));
}

/// Releases the lock that the request with the id turned into, for an agent that has let go of the request.
pub(crate) fn release_lock_with_id(id: EntryId, agent: AgentId) {
    let mut table = lock();
    let held_lock = table.managers.iter().find_map(|(key, manager)| {
        manager
            .held_lock_set
            .iter()
            .find(|lock| lock.id == id)
            .map(|lock| (key.clone(), lock.name.clone()))
    });
    let Some((key, name)) = held_lock else {
        return;
    };
    if let Some(manager) = table.managers.get_mut(&key) {
        manager.held_lock_set.retain(|lock| lock.id != id);
    }
    table.process_lock_request_queue(&key, &name, Some(agent));
}

/// Steps 2 to 7 of aborting the request, https://w3c.github.io/web-locks/#abort-the-request, by the agent of the
/// request. Returns whether the request was still in its queue.
pub(crate) fn abort_request(key: &LockManagerKey, name: &Utf16String, id: EntryId, agent: AgentId) -> bool {
    let mut table = lock();

    // 2. Let manager be request’s manager.
    // 3. Let name be request’s name.
    // 4. Let queueMap be manager’s lock request queue map.
    // 5. Let queue be the result of getting the lock request queue from queueMap for name.
    let Some(queue) = table
        .managers
        .get_mut(key)
        .and_then(|manager| manager.lock_request_queue_map.get_mut(name))
    else {
        return false;
    };

    // 6. Remove request from queue.
    let length = queue.len();
    queue.retain(|request| request.id != id);
    let was_queued = queue.len() != length;

    // 7. Process the lock request queue queue.
    table.process_lock_request_queue(key, name, Some(agent));
    was_queued
}

/// Steps 2 to 5 of snapshotting the lock state, https://w3c.github.io/web-locks/#snapshot-the-lock-state, which return
/// held and pending.
pub(crate) fn snapshot_lock_state(key: &LockManagerKey) -> (Vec<LockInfo>, Vec<LockInfo>) {
    let table = lock();
    let Some(manager) = table.managers.get(key) else {
        return (Vec::new(), Vec::new());
    };

    // 2. Let pending be a new list.
    // 3. For each queue of manager’s lock request queue map’s values:
    //    1. For each request of queue:
    //       1. Append «[ "name" → request’s name, "mode" → request’s mode, "clientId" → request’s clientId ]» to pending.
    let pending = manager
        .lock_request_queue_map
        .values()
        .flatten()
        .map(Entry::info)
        .collect();

    // 4. Let held be a new list.
    // 5. For each lock of manager’s held lock set:
    //    1. Append «[ "name" → lock’s name, "mode" → lock’s mode, "clientId" → lock’s clientId ]» to held.
    let held = manager.held_lock_set.iter().map(Entry::info).collect();

    (held, pending)
}

/// Terminates the remaining locks and requests of the agent's client, https://w3c.github.io/web-locks/#termination-of-locks,
/// by that agent.
pub(crate) fn terminate_remaining_locks_and_requests_of_client(agent: AgentId, client_id: &Utf16String) {
    lock().terminate_remaining_locks_and_requests(
        |entry| entry.agent == agent && entry.client_id == *client_id,
        Some(agent),
    );
}

/// Terminates the remaining locks and requests of the agent, https://w3c.github.io/web-locks/#termination-of-locks, from
/// any thread. The table takes no requests from the agent after this.
pub(crate) fn terminate_remaining_locks_and_requests_of_agent(agent: AgentId) {
    let mut table = lock();
    if table.agents.remove(&agent).is_none() {
        return;
    }
    table.terminate_remaining_locks_and_requests(|entry| entry.agent == agent, None);
}
