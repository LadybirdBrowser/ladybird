/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/SecurityPolicyViolationEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::ContentSecurityPolicy {

class SecurityPolicyViolationEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(SecurityPolicyViolationEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SecurityPolicyViolationEvent);

public:
    [[nodiscard]] static GC::Ref<SecurityPolicyViolationEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::SecurityPolicyViolationEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<SecurityPolicyViolationEvent>> construct_impl(JS::Realm&, Utf16FlyString const& event_name, Bindings::SecurityPolicyViolationEventInit const& event_init);

    virtual ~SecurityPolicyViolationEvent() override;

    Utf16String const& document_uri() const { return m_document_uri; }
    Utf16String const& referrer() const { return m_referrer; }
    Utf16String const& blocked_uri() const { return m_blocked_uri; }
    Utf16String const& violated_directive() const { return m_violated_directive; }
    Utf16String const& effective_directive() const { return m_effective_directive; }
    Utf16String const& original_policy() const { return m_original_policy; }
    Utf16String const& source_file() const { return m_source_file; }
    Utf16String const& sample() const { return m_sample; }
    Bindings::SecurityPolicyViolationEventDisposition disposition() const { return m_disposition; }
    u16 status_code() const { return m_status_code; }
    u32 line_number() const { return m_line_number; }
    u32 column_number() const { return m_column_number; }

private:
    SecurityPolicyViolationEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::SecurityPolicyViolationEventInit const&);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_document_uri;
    Utf16String m_referrer;
    Utf16String m_blocked_uri;
    Utf16String m_violated_directive;
    Utf16String m_effective_directive;
    Utf16String m_original_policy;
    Utf16String m_source_file;
    Utf16String m_sample;
    Bindings::SecurityPolicyViolationEventDisposition m_disposition { Bindings::SecurityPolicyViolationEventDisposition::Enforce };
    u16 m_status_code { 0 };
    u32 m_line_number { 0 };
    u32 m_column_number { 0 };
};

}
