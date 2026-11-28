/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Permissions, https://w3c.github.io/permissions/#permissions-interface

use core::cell::RefCell;
use core::ffi::c_void;
use core::ptr::NonNull;
use libjs_rust::api::ref_cell_for_trace;

use ak::Utf16String;
use libjs_rust::api::{
    EmbedderClass, EmbedderValue, ErrorKind, ErrorType, Gc, Object, Throw, Trace, Value, Visitor, Vm,
};

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
    fn web_permissions_api_request_permission_to_use(
        settings_object: *mut c_void,
        name: u8,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void, u8),
    );
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
    permission_state_from(unsafe { web_permissions_api_permission_state(settings.as_ptr(), name as u8) })
}

fn permission_state_from(state: u8) -> PermissionState {
    match state {
        0 => PermissionState::Granted,
        1 => PermissionState::Denied,
        2 => PermissionState::Prompt,
        _ => unreachable!("LibWeb returns a PermissionState"),
    }
}

/// Steps that run once with the state that requesting permission to use settled on.
trait DecisionSteps: Trace {
    fn run(&self, vm: &Vm, state: PermissionState);
}

struct CapturedDecisionSteps<C> {
    captures: C,
    steps: fn(&Vm, C, PermissionState),
}

// SAFETY: The steps are a plain function, which holds no cells.
unsafe impl<C: Trace> Trace for CapturedDecisionSteps<C> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        self.captures.trace(visitor);
    }
}

impl<C: Trace + Clone + 'static> DecisionSteps for CapturedDecisionSteps<C> {
    fn run(&self, vm: &Vm, state: PermissionState) {
        (self.steps)(vm, self.captures.clone(), state);
    }
}

/// The cell that keeps the steps of a permission request and what they capture alive until the request settles.
struct PendingDecision(RefCell<Option<Box<dyn DecisionSteps>>>);

// SAFETY: The steps that have not run are all the cells this reaches.
unsafe impl Trace for PendingDecision {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        // SAFETY: Nothing takes the steps while a collection traces them.
        if let Some(steps) = unsafe { ref_cell_for_trace(&self.0) } {
            steps.trace(visitor);
        }
    }
}

static PENDING_DECISION_CLASS: EmbedderClass<PendingDecision> = EmbedderClass::new("PendingDecision");

impl EmbedderValue for PendingDecision {
    const CLASS: &'static EmbedderClass<Self> = &PENDING_DECISION_CLASS;
}

unsafe extern "C" fn run_pending_decision(vm: *mut c_void, cell: *mut c_void, state: u8) {
    // SAFETY: LibWeb passes the VM and the cell that request_permission_to_use() made, which it kept alive.
    let (vm, cell) = unsafe {
        (
            &*vm.cast::<Vm>(),
            PENDING_DECISION_CLASS
                .downcast(NonNull::new(cell).expect("LibWeb passes the cell"))
                .expect("LibWeb passes a PendingDecision"),
        )
    };
    // NB: The cell keeps what the steps capture alive while they run, and lets go of it once they have.
    cell.0
        .borrow()
        .as_ref()
        .expect("a permission request settles once")
        .run(vm, permission_state_from(state));
    cell.0.take();
}

// https://w3c.github.io/permissions/#dfn-request-permission-to-use
pub(crate) fn request_permission_to_use<C: Trace + Clone + 'static>(
    vm: &Vm,
    name: PermissionName,
    settings: &CppEnvironmentSettingsObject,
    captures: C,
    steps: fn(&Vm, C, PermissionState),
) {
    let cell = PENDING_DECISION_CLASS.allocate(vm, |_| {
        let steps: Box<dyn DecisionSteps> = Box::new(CapturedDecisionSteps { captures, steps });
        PendingDecision(RefCell::new(Some(steps)))
    });
    // SAFETY: The settings object and the cell are live.
    unsafe {
        web_permissions_api_request_permission_to_use(
            settings.as_ptr(),
            name as u8,
            cell.as_ptr().cast(),
            run_pending_decision,
        );
    }
}

// https://w3c.github.io/permissions/#permissions-interface
#[derive(Trace)]
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
        let relevant_realm = cx.realm();
        let relevant_global_object = relevant_realm.global_object();
        let current_realm = vm.current_realm().expect("query() runs in a realm");
        let current_global_object = current_realm.global_object();

        // 1. If this's relevant global object is a Window object, then:
        if window_from_global_object(relevant_global_object).is_some() {
            // 1. If the current settings object's associated Document is not fully active, return a promise rejected with an "InvalidStateError" DOMException.
            let window = window_from_global_object(current_global_object)
                .expect("the objects of a window are reachable only from windows");
            if !window.associated_document().is_fully_active() {
                let error = exception_value(
                    vm,
                    current_realm,
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
        // AD-HOC: The base PermissionDescriptor isn't converted a second time: That would read name again, which could
        //         then name a permission that isn't supported.
        let typed_descriptor = root_desc;

        // 8. Return promise and continue in parallel:
        // FIXME: Continue in parallel.
        {
            // 1. Let status be create a PermissionStatus with typedDescriptor.
            let status = PermissionStatus::create(vm, relevant_realm, typed_descriptor);

            // 2. Let query be status's [[query]] internal slot.
            let query = status.query();

            // 3. Run query's name's permission query algorithm, passing query and status.
            // FIXME: For now, only the base PermissionDescriptor is supported so there is only one permission query algorithm.
            // NB: The status was registered for changes when it was created, so a change from here on reaches it.
            // AD-HOC: The state is read with this's relevant settings object, as the changes the status hears of later
            //         are, rather than with the current settings object that permission state defaults to.
            permission_query_algorithm(query, &status, &relevant_settings_object(relevant_global_object));

            // 4. Queue a global task on the permissions task source with this's relevant global object to resolve promise with status.
            queue_a_global_task(
                vm,
                relevant_global_object,
                TaskSource::Permissions,
                (relevant_realm, promise, status),
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
pub(crate) fn permission_query_algorithm(
    permission_desc: &PermissionDescriptor,
    status: &PermissionStatus,
    settings: &CppEnvironmentSettingsObject,
) {
    // 1. Set status's state to permissionDesc's permission state.
    let name = PermissionName::from_name(&permission_desc.name).expect("the name is supported");
    status.set_state(permission_state(name, settings));
}
