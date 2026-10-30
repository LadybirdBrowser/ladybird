/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The serialization steps of QuotaExceededError, https://webidl.spec.whatwg.org/#quotaexceedederror, which LibWeb
//! implements in C++ (Libraries/LibWeb/WebIDL/QuotaExceededError.cpp).

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Object, Realm};

use super::dom_exception::{self, DOMExceptionFields};
use crate::bindings::WebIdlResult;
use crate::html::structured_serialize::cpp::StructuredSerializeReader;
use crate::html::structured_serialize::writer::StructuredSerializeWriter;

unsafe extern "C" {
    fn web_quota_exceeded_error_quota_and_requested(
        error: *mut c_void,
        has_quota: &mut bool,
        quota: &mut f64,
        has_requested: &mut bool,
        requested: &mut f64,
    );
    #[allow(clippy::too_many_arguments, reason = "the fields of the error pass one by one")]
    fn web_quota_exceeded_error_create(
        realm: *mut c_void,
        name: usize,
        message: usize,
        has_quota: bool,
        quota: f64,
        has_requested: bool,
        requested: f64,
    ) -> *mut c_void;
}

/// The fields of a QuotaExceededError: those of a DOMException, its quota and its requested.
pub(crate) struct QuotaExceededErrorFields {
    pub(crate) dom_exception: DOMExceptionFields,
    pub(crate) quota: Option<f64>,
    pub(crate) requested: Option<f64>,
}

impl QuotaExceededErrorFields {
    /// The fields of the QuotaExceededError.
    pub(crate) fn of(error: Gc<Object>) -> Self {
        let (mut has_quota, mut quota, mut has_requested, mut requested) = (false, 0.0, false, 0.0);
        // SAFETY: The object is a live QuotaExceededError.
        unsafe {
            web_quota_exceeded_error_quota_and_requested(
                error.as_ptr().cast(),
                &mut has_quota,
                &mut quota,
                &mut has_requested,
                &mut requested,
            );
        }
        Self {
            dom_exception: DOMExceptionFields::of(error),
            quota: has_quota.then_some(quota),
            requested: has_requested.then_some(requested),
        }
    }
}

/// A new QuotaExceededError with the fields, created in the realm.
pub(crate) fn create(realm: Gc<Realm>, fields: &QuotaExceededErrorFields) -> Gc<Object> {
    // SAFETY: The realm is live, and the call borrows the strings.
    let error = unsafe {
        web_quota_exceeded_error_create(
            realm.as_ptr().cast(),
            fields.dom_exception.name.raw_identity(),
            fields.dom_exception.message.raw_identity(),
            fields.quota.is_some(),
            fields.quota.unwrap_or_default(),
            fields.requested.is_some(),
            fields.requested.unwrap_or_default(),
        )
    };
    // SAFETY: LibWeb returns a live object.
    unsafe { Gc::from_non_null(NonNull::new(error.cast()).expect("LibWeb creates the error")) }
}

// https://webidl.spec.whatwg.org/#ref-for-quotaexceedederror⑦
pub(crate) fn serialization_steps(value: &QuotaExceededErrorFields, serialized: &StructuredSerializeWriter) {
    // 1. Run the DOMException serialization steps given value and serialized.
    dom_exception::serialization_steps(&value.dom_exception, serialized);

    // 2. Set serialized.[[Quota]] to value’s quota.
    serialized.encode_optional_f64(value.quota);

    // 3. Set serialized.[[Requested]] to value’s requested.
    serialized.encode_optional_f64(value.requested);
}

// https://webidl.spec.whatwg.org/#ref-for-quotaexceedederror⑦
pub(crate) fn deserialization_steps(serialized: StructuredSerializeReader) -> WebIdlResult<QuotaExceededErrorFields> {
    // 1. Run the DOMException deserialization steps given serialized and value.
    let dom_exception = dom_exception::deserialization_steps(serialized)?;

    // 2. Set value’s quota to serialized.[[Quota]].
    let quota = serialized.decode_optional_f64()?;

    // 3. Set value’s requested to serialized.[[Requested]].
    let requested = serialized.decode_optional_f64()?;

    Ok(QuotaExceededErrorFields {
        dom_exception,
        quota,
        requested,
    })
}
