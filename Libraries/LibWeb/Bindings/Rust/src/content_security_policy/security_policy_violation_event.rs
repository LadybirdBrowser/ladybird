/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SecurityPolicyViolationEvent, https://w3c.github.io/webappsec-csp/#securitypolicyviolationevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate, realm_arguments};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

// https://w3c.github.io/webappsec-csp/#enumdef-securitypolicyviolationeventdisposition
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum SecurityPolicyViolationEventDisposition {
    Enforce,
    Report,
}

/// The dictionary SecurityPolicyViolationEventInit of ContentSecurityPolicy/SecurityPolicyViolationEvent.idl.
pub(crate) struct SecurityPolicyViolationEventInit {
    pub(crate) parent: EventInit,
    pub(crate) document_uri: UsvString,
    pub(crate) referrer: UsvString,
    pub(crate) blocked_uri: UsvString,
    pub(crate) violated_directive: Utf16String,
    pub(crate) effective_directive: Utf16String,
    pub(crate) original_policy: Utf16String,
    pub(crate) source_file: UsvString,
    pub(crate) sample: Utf16String,
    pub(crate) disposition: SecurityPolicyViolationEventDisposition,
    pub(crate) status_code: u16,
    pub(crate) line_number: u32,
    pub(crate) column_number: u32,
}

/// The members of SecurityPolicyViolationEventInit that C++ lends to create an event.
#[repr(C)]
pub struct FfiSecurityPolicyViolationEventInit {
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    document_uri: usize,
    referrer: usize,
    blocked_uri: usize,
    violated_directive: usize,
    effective_directive: usize,
    original_policy: usize,
    source_file: usize,
    sample: usize,
    disposition: SecurityPolicyViolationEventDisposition,
    status_code: u16,
    line_number: u32,
    column_number: u32,
}

struct SecurityPolicyViolationEventAttributes {
    document_uri: UsvString,
    referrer: UsvString,
    blocked_uri: UsvString,
    violated_directive: Utf16String,
    effective_directive: Utf16String,
    original_policy: Utf16String,
    source_file: UsvString,
    sample: Utf16String,
    disposition: SecurityPolicyViolationEventDisposition,
    status_code: u16,
    line_number: u32,
    column_number: u32,
}

// https://w3c.github.io/webappsec-csp/#securitypolicyviolationevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SecurityPolicyViolationEvent {
    event: CppEvent,
    #[gc(untraced)]
    attributes: SecurityPolicyViolationEventAttributes,
}

impl Wrappable for SecurityPolicyViolationEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl SecurityPolicyViolationEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: SecurityPolicyViolationEventInit,
    ) -> Ref<SecurityPolicyViolationEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| SecurityPolicyViolationEvent {
            event,
            attributes: SecurityPolicyViolationEventAttributes {
                document_uri: event_init.document_uri,
                referrer: event_init.referrer,
                blocked_uri: event_init.blocked_uri,
                violated_directive: event_init.violated_directive,
                effective_directive: event_init.effective_directive,
                original_policy: event_init.original_policy,
                source_file: event_init.source_file,
                sample: event_init.sample,
                disposition: event_init.disposition,
                status_code: event_init.status_code,
                line_number: event_init.line_number,
                column_number: event_init.column_number,
            },
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: SecurityPolicyViolationEventInit,
    ) -> WebIdlResult<Ref<SecurityPolicyViolationEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn document_uri(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.attributes.document_uri.clone())
    }

    pub(crate) fn referrer(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.attributes.referrer.clone())
    }

    pub(crate) fn blocked_uri(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.attributes.blocked_uri.clone())
    }

    pub(crate) fn effective_directive(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.attributes.effective_directive.clone())
    }

    pub(crate) fn violated_directive(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.attributes.violated_directive.clone())
    }

    pub(crate) fn original_policy(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.attributes.original_policy.clone())
    }

    pub(crate) fn source_file(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.attributes.source_file.clone())
    }

    pub(crate) fn sample(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.attributes.sample.clone())
    }

    pub(crate) fn disposition(&self, _: &mut CallContext) -> WebIdlResult<SecurityPolicyViolationEventDisposition> {
        Ok(self.attributes.disposition)
    }

    pub(crate) fn status_code(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        Ok(self.attributes.status_code)
    }

    pub(crate) fn line_number(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.attributes.line_number)
    }

    pub(crate) fn column_number(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.attributes.column_number)
    }
}

/// The Event of a new SecurityPolicyViolationEvent in the realm, for the implementation to fire.
///
/// # Safety
///
/// The VM and the realm must be live. The type and the strings of the dictionary are lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_security_policy_violation_event_create(
    vm: *mut c_void,
    realm: *mut c_void,
    r#type: usize,
    event_init: &FfiSecurityPolicyViolationEventInit,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller lends each string for the call.
    let string = |raw| unsafe { Utf16String::from_raw_borrowed(raw) };
    let usv_string = |raw| UsvString::from_utf16_string(&string(raw));
    let event_init = SecurityPolicyViolationEventInit {
        parent: EventInit {
            bubbles: event_init.bubbles,
            cancelable: event_init.cancelable,
            composed: event_init.composed,
        },
        document_uri: usv_string(event_init.document_uri),
        referrer: usv_string(event_init.referrer),
        blocked_uri: usv_string(event_init.blocked_uri),
        violated_directive: string(event_init.violated_directive),
        effective_directive: string(event_init.effective_directive),
        original_policy: string(event_init.original_policy),
        source_file: usv_string(event_init.source_file),
        sample: string(event_init.sample),
        disposition: event_init.disposition,
        status_code: event_init.status_code,
        line_number: event_init.line_number,
        column_number: event_init.column_number,
    };
    // SAFETY: The caller lends the type for the call.
    let r#type: Utf16String = unsafe { Utf16FlyString::from_raw_borrowed(r#type) }.into();
    SecurityPolicyViolationEvent::create(vm, realm, &r#type, event_init)
        .event()
        .as_ptr()
}
