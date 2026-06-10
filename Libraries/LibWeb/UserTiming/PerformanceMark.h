/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct PerformanceMarkOptions;

}

namespace Web::UserTiming {

struct PerformanceMarkOptions {
    Optional<HTML::SerializationRecord> detail {};
    Optional<HighResolutionTime::DOMHighResTimeStamp> start_time {};
};

// https://w3c.github.io/user-timing/#dom-performancemark
class PerformanceMark final : public PerformanceTimeline::PerformanceEntry {
    WEB_WRAPPABLE(PerformanceMark, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformanceMark);

public:
    virtual ~PerformanceMark();

    [[nodiscard]] static GC::Ref<PerformanceMark> create(String const& mark_name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> create_with_options(String const& mark_name, PerformanceMarkOptions const&, bool is_window_context, HighResolutionTime::DOMHighResTimeStamp default_start_time);
    [[nodiscard]] static WebIDL::ExceptionOr<PerformanceMarkOptions> options_from_bindings(JS::Realm&, Bindings::PerformanceMarkOptions const&);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> create_for_constructor(JS::Realm&, String const& mark_name, Bindings::PerformanceMarkOptions const&);

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
    PerformanceMark(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://w3c.github.io/user-timing/#dom-performancemark-detail
    Optional<HTML::SerializationRecord> m_detail;
};

}
