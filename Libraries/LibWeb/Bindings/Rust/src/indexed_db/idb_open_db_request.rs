/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBOpenDBRequest, https://w3c.github.io/IndexedDB/#idbopendbrequest

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::indexed_db::idb_request::{IDBRequest, IDBRequestSource};

// https://w3c.github.io/IndexedDB/#idbopendbrequest
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBOpenDBRequest {
    request: IDBRequest,
}

impl Inherits<IDBRequest> for IDBOpenDBRequest {
    fn parent(&self) -> &IDBRequest {
        &self.request
    }
}

impl Wrappable for IDBOpenDBRequest {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.request.cpp_parent()
    }
}

impl GetTheParent for IDBOpenDBRequest {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        // https://w3c.github.io/IndexedDB/#open-requests
        // An open request’s get the parent algorithm returns null.
        None
    }
}

impl IDBOpenDBRequest {
    // NOTE: The source of an open request is always null.
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<IDBOpenDBRequest> {
        let request = allocate(vm, |_| IDBOpenDBRequest {
            request: IDBRequest::new(realm, IDBRequestSource::Empty),
        });
        request.request.event_target().set_owner(request);
        request
    }
}
