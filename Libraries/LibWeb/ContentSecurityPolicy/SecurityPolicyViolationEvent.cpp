/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/ContentSecurityPolicy/SecurityPolicyViolationEvent.h>

namespace Web::ContentSecurityPolicy {

GC_DEFINE_ALLOCATOR(SecurityPolicyViolationEvent);

GC::Ref<SecurityPolicyViolationEvent> SecurityPolicyViolationEvent::create(FlyString const& event_name, SecurityPolicyViolationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<SecurityPolicyViolationEvent>(event_name, event_init, time_stamp);
}

SecurityPolicyViolationEvent::SecurityPolicyViolationEvent(FlyString const& event_name, SecurityPolicyViolationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(event_name, event_init, time_stamp)
    , m_document_uri(event_init.document_uri)
    , m_referrer(event_init.referrer)
    , m_blocked_uri(event_init.blocked_uri)
    , m_violated_directive(event_init.violated_directive)
    , m_effective_directive(event_init.effective_directive)
    , m_original_policy(event_init.original_policy)
    , m_source_file(event_init.source_file)
    , m_sample(event_init.sample)
    , m_disposition(event_init.disposition)
    , m_status_code(event_init.status_code)
    , m_line_number(event_init.line_number)
    , m_column_number(event_init.column_number)
{
}

SecurityPolicyViolationEvent::~SecurityPolicyViolationEvent() = default;

}
