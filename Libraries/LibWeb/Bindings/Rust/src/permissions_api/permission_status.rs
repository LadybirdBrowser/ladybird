/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PermissionStatus, https://w3c.github.io/permissions/#permissionstatus-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::{CallContext, WebIdlResult};
use crate::permissions::permissions::PermissionName;
use crate::permissions_api::permission_store::register_status;
use crate::permissions_api::permissions::{PermissionDescriptor, permission_query_algorithm, permission_state};

unsafe extern "C" {
    fn web_html_window_or_worker_global_scope_register_permission_status(
        global_object: *mut c_void,
        status: *mut c_void,
    );
}

// https://w3c.github.io/permissions/#dom-permissionstate
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum PermissionState {
    Granted,
    Denied,
    Prompt,
}

// https://w3c.github.io/permissions/#permissionstatus-interface
#[derive(Trace)]
pub(crate) struct PermissionStatus {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    #[gc(untraced)]
    name: Utf16String,
    #[gc(untraced)]
    permission_name: PermissionName,
    #[gc(untraced)]
    state: Cell<PermissionState>,

    // https://w3c.github.io/permissions/#dfn-query
    #[gc(untraced)]
    query: PermissionDescriptor,
}

impl Wrappable for PermissionStatus {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for PermissionStatus {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl PermissionStatus {
    // https://w3c.github.io/permissions/#dfn-create-a-permissionstatus
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, permission_desc: PermissionDescriptor) -> Ref<Self> {
        // 1. Let name be permissionDesc's name.
        let name = permission_desc.name.clone();

        // 2. Assert: The feature identified by name is supported by the user agent.
        let permission_name = PermissionName::from_name(&name).expect("the name is supported");

        // 3. Let status be a new instance of the permission result type identified by name:
        // FIXME: A permission result type is a PermissionStatus or one of its subtypes.
        // If unspecified, this defaults to PermissionStatus. No subtypes exist yet in Ladybird.
        // 1. Initialize status's [[query]] internal slot to permissionDesc.
        // 2. Initialize status's name to name.
        let status = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            realm,
            name,
            permission_name,
            state: Cell::new(PermissionState::Prompt),
            query: permission_desc,
        });
        status.event_target.set_owner(status);

        // NB: The store tells every status when a permission changes, and the global keeps a status alive while it has a
        //     change listener.
        register_status(vm, status);
        // SAFETY: The global object and the EventTarget are live.
        unsafe {
            web_html_window_or_worker_global_scope_register_permission_status(
                realm.global_object().as_ptr().cast(),
                status.event_target.as_ptr(),
            );
        }

        // 4. Return status.
        status
    }

    // https://w3c.github.io/permissions/#dom-permissionstatus-state
    pub(crate) fn state(&self, _: &mut CallContext) -> WebIdlResult<PermissionState> {
        Ok(self.state.get())
    }

    pub(crate) fn set_state(&self, state: PermissionState) {
        self.state.set(state);
    }

    // https://w3c.github.io/permissions/#dom-permissionstatus-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.name.clone())
    }

    pub(crate) fn query(&self) -> &PermissionDescriptor {
        &self.query
    }

    // https://w3c.github.io/permissions/#PermissionStatus-update
    pub(crate) fn update_from_store(this: Ref<Self>, vm: &Vm) {
        let global = this.realm.global_object();

        // 1. If this's relevant global object is a Window object, then:
        if let Some(window) = window_from_global_object(global) {
            // 1. Let document be status's relevant global object's associated Document.
            // 2. If document is null or document is not fully active, terminate this algorithm.
            if !window.associated_document().is_fully_active() {
                return;
            }
        }

        // NB: The store tells every status of a change, and only those whose state it moved run these steps.
        let settings = relevant_settings_object(global);
        if permission_state(this.permission_name, &settings) == this.state.get() {
            return;
        }

        // 2. Let query be status's [[query]] internal slot.
        let query = this.query();

        // 3. Run query's name's permission query algorithm, passing query and status.
        permission_query_algorithm(query, &this, &settings);

        // 4. Queue a task on the permissions task source to fire an event named change at status.
        queue_a_global_task(vm, global, TaskSource::Permissions, this, |_, this| {
            let event = CppEvent::create(this.realm, "change", false, false, false);
            this.event_target.dispatch_event(&event);
        });
    }
}
