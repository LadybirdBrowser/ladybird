/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The statuses of the permission store, https://w3c.github.io/permissions/#dfn-permission-store

use core::cell::{Cell, RefCell};

use libjs_rust::api::{EmbedderCell, GcWeak, MarkedVec, Vm};

use crate::bindings::interface::{Ref, Wrapped};
use crate::permissions_api::permission_status::PermissionStatus;

thread_local! {
    static PERMISSION_STATUSES: RefCell<Vec<GcWeak<EmbedderCell<Wrapped<PermissionStatus>>>>> =
        const { RefCell::new(Vec::new()) };

    // The newest snapshot that the statuses of this agent have seen.
    static REVISION: Cell<Option<u64>> = const { Cell::new(None) };
}

pub(crate) fn register_status(vm: &Vm, status: Ref<PermissionStatus>) {
    PERMISSION_STATUSES.with_borrow_mut(|statuses| statuses.push(GcWeak::new(vm.heap(), status)));
}

/// Reads the state of every status of the thread's agent again from the permission store, once the store has applied
/// the snapshot of the revision. Returns whether the statuses heard of a change.
pub(crate) fn update_statuses_of_this_agent(vm: &Vm, revision: u64, store_changed: bool) -> bool {
    // NB: The agent that got here first made the change, and the others still have to hear of it. An agent has heard of a
    //     revision only once its statuses have, which is not the case for one that it fetched without telling them.
    let is_unseen = REVISION.get().is_none_or(|seen| revision > seen);
    if is_unseen {
        REVISION.set(Some(revision));
    }
    if !store_changed && !is_unseen {
        return false;
    }

    // NB: Updating a status allocates, and a collection then could clear another status's weak reference, so every
    //     live status is rooted first.
    let statuses = MarkedVec::new(vm);
    PERMISSION_STATUSES.with_borrow_mut(|weak_statuses| {
        weak_statuses.retain(|status| status.get().is_some());
        for status in weak_statuses.iter().filter_map(GcWeak::get) {
            statuses.push(status);
        }
    });
    for status in statuses.values().iter() {
        PermissionStatus::update_from_store(*status, vm);
    }
    true
}

/// For an agent that goes away, whose statuses have to go with its heap.
pub(crate) fn forget_statuses_of_this_agent() {
    PERMISSION_STATUSES.with_borrow_mut(Vec::clear);
    REVISION.set(None);
}
