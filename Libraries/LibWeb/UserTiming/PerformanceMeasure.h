/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UserTiming {

// https://w3c.github.io/user-timing/#dom-performancemeasure
class PerformanceMeasure final : public PerformanceTimeline::PerformanceEntry {
    WEB_WRAPPABLE(PerformanceMeasure, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformanceMeasure);

public:
    virtual ~PerformanceMeasure();

    [[nodiscard]] static GC::Ref<PerformanceMeasure> create(String const& measure_name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail);

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    // NOTE: The empty state represents Infinite size.
    static Optional<u64> max_buffer_size() { return OptionalNone {}; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual FlyString const& entry_type() const override;

    WebIDL::ExceptionOr<JS::Value> detail(JS::Realm&) const;

private:
    PerformanceMeasure(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail);

    // https://w3c.github.io/user-timing/#dom-performancemeasure-detail
    Optional<HTML::SerializationRecord> m_detail;
};

}
