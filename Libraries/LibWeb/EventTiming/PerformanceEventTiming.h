/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/PerformanceEventTiming.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::EventTiming {

// https://w3c.github.io/event-timing/#sec-performance-event-timing
class PerformanceEventTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_RUST_INTERFACE(PerformanceEventTiming, performance_event_timing)

public:
    static GC::Ref<PerformanceEventTiming> create(DOM::Event const&, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id);

    // A copy of this entry, with the given entry type.
    GC::Ref<PerformanceEventTiming> copy_with_entry_type(Utf16FlyString const& entry_type) const;

    bool has_duration() const;
    void set_duration(HighResolutionTime::DOMHighResTimeStamp);
    void set_processing_end(HighResolutionTime::DOMHighResTimeStamp);
    void set_event_target(GC::Ptr<DOM::Node>);
    void set_target_selector(Utf16String);
    WebIDL::UnsignedLongLong interaction_id() const;
    void set_interaction_id(WebIDL::UnsignedLongLong);
};

}
