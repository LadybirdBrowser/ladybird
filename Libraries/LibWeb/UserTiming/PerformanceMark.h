/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct PerformanceMarkOptions;

}

namespace Web::UserTiming {

// https://w3c.github.io/user-timing/#dom-performancemark
class PerformanceMark final : public PerformanceTimeline::PerformanceEntry {
    WEB_WRAPPABLE(PerformanceMark, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformanceMark);

public:
    virtual ~PerformanceMark();

    static WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> create_for_constructor(JS::Object&, Utf16String const& mark_name, Bindings::PerformanceMarkOptions const& mark_options = {});

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    // NOTE: The empty state represents Infinite size.
    static Optional<u64> max_buffer_size() { return OptionalNone {}; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual Utf16FlyString const& entry_type() const override;

    WebIDL::ExceptionOr<JS::Value> detail(JS::Object const& relevant_global_object) const;

private:
    PerformanceMark(Utf16String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://w3c.github.io/user-timing/#dom-performancemark-detail
    JS::Value m_detail { JS::js_null() };
};

}
