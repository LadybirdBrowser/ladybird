/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/SecurityPolicyViolationEvent.h>

namespace Web::ContentSecurityPolicy {

// Matches the Rust FfiSecurityPolicyViolationEventInit.
struct FfiSecurityPolicyViolationEventInit {
    bool bubbles;
    bool cancelable;
    bool composed;
    FlatPtr document_uri;
    FlatPtr referrer;
    FlatPtr blocked_uri;
    FlatPtr violated_directive;
    FlatPtr effective_directive;
    FlatPtr original_policy;
    FlatPtr source_file;
    FlatPtr sample;
    Bindings::SecurityPolicyViolationEventDisposition disposition;
    u16 status_code;
    u32 line_number;
    u32 column_number;
};

extern "C" {
// Defined in Bindings/Rust/src/content_security_policy/security_policy_violation_event.rs.
DOM::Event* web_rust_security_policy_violation_event_create(JSVM*, void* realm, FlatPtr event_name, FfiSecurityPolicyViolationEventInit const*);
}

GC::Ref<DOM::Event> create_security_policy_violation_event(JS::Realm& realm, Utf16FlyString const& event_name, SecurityPolicyViolationEventInit const& event_init)
{
    FfiSecurityPolicyViolationEventInit ffi_event_init {
        .bubbles = event_init.bubbles,
        .cancelable = event_init.cancelable,
        .composed = event_init.composed,
        .document_uri = event_init.document_uri.raw_identity(),
        .referrer = event_init.referrer.raw_identity(),
        .blocked_uri = event_init.blocked_uri.raw_identity(),
        .violated_directive = event_init.violated_directive.raw_identity(),
        .effective_directive = event_init.effective_directive.raw_identity(),
        .original_policy = event_init.original_policy.raw_identity(),
        .source_file = event_init.source_file.raw_identity(),
        .sample = event_init.sample.raw_identity(),
        .disposition = event_init.disposition,
        .status_code = event_init.status_code,
        .line_number = event_init.line_number,
        .column_number = event_init.column_number,
    };
    return *web_rust_security_policy_violation_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, event_name.raw_identity(), &ffi_event_init);
}

}
