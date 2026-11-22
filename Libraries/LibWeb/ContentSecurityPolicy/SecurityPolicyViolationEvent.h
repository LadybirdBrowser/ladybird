/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Bindings/SecurityPolicyViolationEvent.h>

namespace Web::ContentSecurityPolicy {

struct SecurityPolicyViolationEventInit : public DOM::EventInit {
    Utf16String document_uri;
    Utf16String referrer;
    Utf16String blocked_uri;
    Utf16String violated_directive;
    Utf16String effective_directive;
    Utf16String original_policy;
    Utf16String source_file;
    Utf16String sample;
    Bindings::SecurityPolicyViolationEventDisposition disposition { Bindings::SecurityPolicyViolationEventDisposition::Enforce };
    u16 status_code { 0 };
    u32 line_number { 0 };
    u32 column_number { 0 };
};

GC::Ref<DOM::Event> create_security_policy_violation_event(JS::Realm&, Utf16FlyString const& event_name, SecurityPolicyViolationEventInit const&);

}
