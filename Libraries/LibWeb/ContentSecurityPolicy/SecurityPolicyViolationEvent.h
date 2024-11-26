/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/SecurityPolicyViolationEventPrototype.h>
#include <LibWeb/DOM/Event.h>

namespace Web::ContentSecurityPolicy {

struct SecurityPolicyViolationEventInit final : public DOM::EventInit {
    String document_uri;
    String referrer;
    String blocked_uri;
    String violated_directive;
    String effective_directive;
    String original_policy;
    String source_file;
    String sample;
    Bindings::SecurityPolicyViolationEventDisposition disposition { Bindings::SecurityPolicyViolationEventDisposition::Enforce };
    u16 status_code { 0 };
    u32 line_number { 0 };
    u32 column_number { 0 };
};

class SecurityPolicyViolationEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(SecurityPolicyViolationEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SecurityPolicyViolationEvent);

public:
    [[nodiscard]] static GC::Ref<SecurityPolicyViolationEvent> create(JS::Realm&, FlyString const& event_name, SecurityPolicyViolationEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<SecurityPolicyViolationEvent>> construct_impl(JS::Realm&, FlyString const& event_name, SecurityPolicyViolationEventInit const& event_init);

    virtual ~SecurityPolicyViolationEvent() override;

    String const& document_uri() const { return m_document_uri; }
    String const& referrer() const { return m_referrer; }
    String const& blocked_uri() const { return m_blocked_uri; }
    String const& violated_directive() const { return m_violated_directive; }
    String const& effective_directive() const { return m_effective_directive; }
    String const& original_policy() const { return m_original_policy; }
    String const& source_file() const { return m_source_file; }
    String const& sample() const { return m_sample; }
    Bindings::SecurityPolicyViolationEventDisposition disposition() const { return m_disposition; }
    u16 status_code() const { return m_status_code; }
    u32 line_number() const { return m_line_number; }
    u32 column_number() const { return m_column_number; }

private:
    SecurityPolicyViolationEvent(JS::Realm&, FlyString const& event_name, SecurityPolicyViolationEventInit const&);

    virtual void initialize(JS::Realm&) override;

    String m_document_uri;
    String m_referrer;
    String m_blocked_uri;
    String m_violated_directive;
    String m_effective_directive;
    String m_original_policy;
    String m_source_file;
    String m_sample;
    Bindings::SecurityPolicyViolationEventDisposition m_disposition { Bindings::SecurityPolicyViolationEventDisposition::Enforce };
    u16 m_status_code { 0 };
    u32 m_line_number { 0 };
    u32 m_column_number { 0 };
};

}
