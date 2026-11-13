/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Integration with Content Security Policy, https://w3c.github.io/trusted-types/dist/spec/#integration-with-content-security-policy

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm};

unsafe extern "C" {
    fn web_trusted_types_does_sink_require_trusted_types(
        global: *mut c_void,
        sink_group: usize,
        include_report_only_policies: bool,
    ) -> bool;
    fn web_trusted_types_should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
        global: *mut c_void,
        sink: usize,
        sink_group: usize,
        source: usize,
    ) -> bool;
    fn web_trusted_types_should_trusted_type_policy_creation_be_blocked_by_content_security_policy(
        realm: *mut c_void,
        global: *mut c_void,
        policy_name: usize,
        created_policy_names: ak::Span<'_, usize>,
    ) -> bool;
}

// https://w3c.github.io/trusted-types/dist/spec/#does-sink-require-trusted-types
pub(crate) fn does_sink_require_trusted_types(
    global: Gc<Object>,
    sink_group: &Utf16String,
    include_report_only_policies: bool,
) -> bool {
    // SAFETY: The global object and the string are live for the call.
    unsafe {
        web_trusted_types_does_sink_require_trusted_types(
            global.as_ptr().cast(),
            sink_group.raw_identity(),
            include_report_only_policies,
        )
    }
}

/// https://w3c.github.io/trusted-types/dist/spec/#should-block-sink-type-mismatch
pub(crate) fn should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
    global: Gc<Object>,
    sink: &Utf16String,
    sink_group: &Utf16String,
    source: &Utf16String,
) -> bool {
    // SAFETY: The global object and the strings are live for the call.
    unsafe {
        web_trusted_types_should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
            global.as_ptr().cast(),
            sink.raw_identity(),
            sink_group.raw_identity(),
            source.raw_identity(),
        )
    }
}

/// https://w3c.github.io/trusted-types/dist/spec/#should-block-create-policy
pub(crate) fn should_trusted_type_policy_creation_be_blocked_by_content_security_policy(
    realm: Gc<Realm>,
    global: Gc<Object>,
    policy_name: &Utf16String,
    created_policy_names: &[Utf16String],
) -> bool {
    let created_policy_names: Vec<usize> = created_policy_names.iter().map(Utf16String::raw_identity).collect();
    // SAFETY: The realm, the global object and the strings are live for the call.
    unsafe {
        web_trusted_types_should_trusted_type_policy_creation_be_blocked_by_content_security_policy(
            realm.as_ptr().cast(),
            global.as_ptr().cast(),
            policy_name.raw_identity(),
            ak::Span::new(&created_policy_names),
        )
    }
}
