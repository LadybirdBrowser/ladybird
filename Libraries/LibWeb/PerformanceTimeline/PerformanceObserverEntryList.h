/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::PerformanceTimeline {

// https://w3c.github.io/performance-timeline/#performanceobserverentrylist-interface
class PerformanceObserverEntryList final : public Bindings::Wrappable {
    WEB_WRAPPABLE(PerformanceObserverEntryList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(PerformanceObserverEntryList);

public:
    static GC::Ref<PerformanceObserverEntryList> create(Vector<GC::Ref<PerformanceTimeline::PerformanceEntry>>&&);

    virtual ~PerformanceObserverEntryList() override;

    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries() const;
    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries_by_type(String const& type) const;
    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries_by_name(String const& name, Optional<String> type) const;

private:
    explicit PerformanceObserverEntryList(Vector<GC::Ref<PerformanceTimeline::PerformanceEntry>>&&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://w3c.github.io/performance-timeline/#dfn-entry-list
    // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
    // name and type set to null.
    Vector<GC::Ref<PerformanceTimeline::PerformanceEntry>> m_entry_list;
};

ErrorOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> filter_buffer_by_name_and_type(Vector<GC::Ref<PerformanceTimeline::PerformanceEntry>> const& buffer, Optional<String> name, Optional<String> type);

}
