/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::PerformanceTimeline {

// https://w3c.github.io/performance-timeline/#performanceobserverentrylist-interface
class PerformanceObserverEntryList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PerformanceObserverEntryList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(PerformanceObserverEntryList);

public:
    virtual ~PerformanceObserverEntryList() override;

    WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> get_entries() const;
    WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> get_entries_by_type(String const& type) const;
    WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> get_entries_by_name(String const& name, Optional<String> type) const;

private:
    PerformanceObserverEntryList(JS::Realm&, Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>>&&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://w3c.github.io/performance-timeline/#dfn-entry-list
    // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
    // name and type set to null.
    Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>> m_entry_list;
};

ErrorOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> filter_buffer_by_name_and_type(Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>> const& buffer, Optional<String> name, Optional<String> type);

}
