/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::UserTiming {

// https://w3c.github.io/user-timing/#ref-for-dom-performancemarkoptions-1
struct PerformanceMarkOptions {
    JS::Value detail { JS::js_null() };
    Optional<HighResolutionTime::DOMHighResTimeStamp> start_time;
};

// https://w3c.github.io/user-timing/#dom-performancemark
class PerformanceMark final : public PerformanceTimeline::PerformanceEntry {
    WEB_PLATFORM_OBJECT(PerformanceMark, PerformanceTimeline::PerformanceEntry);
    JS_DECLARE_ALLOCATOR(PerformanceMark);

public:
    virtual ~PerformanceMark();

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<PerformanceMark>> construct_impl(JS::Realm&, String const& mark_name, PerformanceMarkOptions const& mark_options = {});

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

    JS::Value detail() const { return m_detail; }

private:
    PerformanceMark(JS::Realm&, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://w3c.github.io/user-timing/#dom-performancemark-detail
    JS::Value m_detail { JS::js_null() };
};

}
