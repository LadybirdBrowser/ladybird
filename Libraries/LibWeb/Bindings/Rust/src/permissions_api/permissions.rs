/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Permissions, https://w3c.github.io/permissions/#permissions-interface

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{ErrorKind, ErrorType, Gc, Object, Throw, Trace, Value, Vm};

#[cfg(web_idl_bindings)]
use crate::bindings::conversions::FromJs;
use crate::bindings::environment_settings_object::{CppEnvironmentSettingsObject, relevant_settings_object};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Wrappable, with_temporary_execution_context, wrap};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, exception_value, queue_a_global_task, reject_promise, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::permissions::permissions::PermissionName;
use crate::permissions_api::permission_status::{PermissionState, PermissionStatus};

unsafe extern "C" {
    fn web_permissions_api_permission_state(settings_object: *mut c_void, name: u8) -> u8;
}

// https://w3c.github.io/permissions/#dom-permissiondescriptor
#[derive(Clone)]
pub(crate) struct PermissionDescriptor {
    pub(crate) name: Utf16String,
}

impl PermissionName {
    pub(crate) fn from_name(name: &Utf16String) -> Option<Self> {
        if *name == "geolocation" {
            return Some(Self::Geolocation);
        }
        if *name == "microphone" {
            return Some(Self::Microphone);
        }
        if *name == "notifications" {
            return Some(Self::Notifications);
        }
        None
    }
}

// https://w3c.github.io/permissions/#dfn-permission-state
pub(crate) fn permission_state(name: PermissionName, settings: &CppEnvironmentSettingsObject) -> PermissionState {
    // SAFETY: The settings object is live.
    match unsafe { web_permissions_api_permission_state(settings.as_ptr(), name as u8) } {
        0 => PermissionState::Granted,
        1 => PermissionState::Denied,
        2 => PermissionState::Prompt,
        _ => unreachable!("LibWeb returns a PermissionState"),
    }
}

// https://w3c.github.io/permissions/#permissions-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Permissions;

impl Wrappable for Permissions {}

fn convert_to_permission_descriptor(vm: &Vm, value: Value) -> Result<PermissionDescriptor, Throw> {
    #[cfg(web_idl_bindings)]
    return PermissionDescriptor::from_js(vm, value);
    #[cfg(not(web_idl_bindings))]
    {
        let _ = (vm, value);
        unreachable!("converting to PermissionDescriptor requires the generated bindings")
    }
}

impl Permissions {
    // https://w3c.github.io/permissions/#query-method
    pub(crate) fn query(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        permission_desc: Gc<Object>,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = vm.current_realm().expect("query() runs in a realm");
        let relevant_global_object = realm.global_object();

        // 1. If this's relevant global object is a Window object, then:
        if let Some(window) = window_from_global_object(relevant_global_object) {
            // 1. If the current settings object's associated Document is not fully active, return a promise rejected with an "InvalidStateError" DOMException.
            if !window.associated_document().is_fully_active() {
                let error = exception_value(
                    vm,
                    realm,
                    Exception::Dom(
                        "InvalidStateError",
                        Utf16String::from_ascii(b"The document is not fully active."),
                    ),
                );
                reject_promise(vm, promise, error);
                return Ok(());
            }
        }

        // 2. Let rootDesc be the object permissionDesc refers to, converted to an IDL value of type PermissionDescriptor.
        // 3. If the conversion throws an exception, return a promise rejected with that exception.
        let root_desc = match convert_to_permission_descriptor(vm, Value::from_object(permission_desc)) {
            Ok(root_desc) => root_desc,
            Err(throw) => {
                reject_promise(vm, promise, throw.value());
                return Ok(());
            }
        };

        // 4. If rootDesc["name"] is not supported, return a promise rejected with a TypeError.
        if PermissionName::from_name(&root_desc.name).is_none() {
            let Err(error) = vm.throw_completion::<()>(
                ErrorKind::TypeError,
                ErrorType::InvalidEnumerationValue,
                &[&root_desc.name, &"PermissionName"],
            ) else {
                unreachable!("throw_completion() throws")
            };
            reject_promise(vm, promise, error.value());
            return Ok(());
        }

        // 5. Let typedDescriptor be the object permissionDesc refers to, converted to an IDL value of rootDesc's name's permission descriptor type.
        // 6. If the conversion throws an exception, return a promise rejected with that exception.
        // FIXME: Support specific permission descriptor types. For now, we only support the base PermissionDescriptor.
        let typed_descriptor = root_desc;

        // 8. Return promise and continue in parallel:
        // FIXME: Continue in parallel.
        {
            // 1. Let status be create a PermissionStatus with typedDescriptor.
            let status = PermissionStatus::create(vm, realm, typed_descriptor);

            // 2. Let query be status's [[query]] internal slot.
            let query = status.query();

            // 3. Run query's name's permission query algorithm, passing query and status.
            // FIXME: For now, only the base PermissionDescriptor is supported so there is only one permission query algorithm.
            // NB: The status was registered for changes when it was created, so a change from here on reaches it.
            permission_query_algorithm(query, &status, &relevant_settings_object(relevant_global_object));

            // 4. Queue a global task on the permissions task source with this's relevant global object to resolve promise with status.
            queue_a_global_task(
                vm,
                relevant_global_object,
                TaskSource::Permissions,
                (realm, promise, status),
                |vm, (realm, promise, status)| {
                    with_temporary_execution_context(realm, || {
                        let wrapper = wrap(&CallContext::new(vm, realm), status);
                        resolve_promise(vm, promise, Value::from_object(wrapper));
                    });
                },
            );
        }

        Ok(())
    }
}

// https://w3c.github.io/permissions/#dfn-permission-query-algorithm
fn permission_query_algorithm(
    permission_desc: &PermissionDescriptor,
    status: &PermissionStatus,
    settings: &CppEnvironmentSettingsObject,
) {
    // 1. Set status's state to permissionDesc's permission state.
    let name = PermissionName::from_name(&permission_desc.name).expect("the name is supported");
    status.set_state(permission_state(name, settings));
}
