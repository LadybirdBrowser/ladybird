/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceObserverEntryListPrototype.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserverEntryList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::PerformanceTimeline {

JS_DEFINE_ALLOCATOR(PerformanceObserverEntryList);

PerformanceObserverEntryList::PerformanceObserverEntryList(JS::Realm& realm, Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>>&& entry_list)
    : Bindings::PlatformObject(realm)
    , m_entry_list(move(entry_list))
{
}

PerformanceObserverEntryList::~PerformanceObserverEntryList() = default;

void PerformanceObserverEntryList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceObserverEntryList);
}

void PerformanceObserverEntryList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_entry_list);
}

// https://www.w3.org/TR/performance-timeline/#dfn-filter-buffer-by-name-and-type
ErrorOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> filter_buffer_by_name_and_type(Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>> const& buffer, Optional<String> name, Optional<String> type)
{
    // 1. Let result be an initially empty list.
    Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>> result;

    // 2. For each PerformanceEntry entry in buffer, run the following steps:
    for (auto const& entry : buffer) {
        // 1. If type is not null and if type is not identical to entry's entryType attribute, continue to next entry.
        if (type.has_value() && type.value() != entry->entry_type())
            continue;

        // 2. If name is not null and if name is not identical to entry's name attribute, continue to next entry.
        if (name.has_value() && name.value() != entry->name())
            continue;

        // 3. append entry to result.
        TRY(result.try_append(entry));
    }

    // 3. Sort results's entries in chronological order with respect to startTime
    quick_sort(result, [](auto const& left_entry, auto const& right_entry) {
        return left_entry->start_time() < right_entry->start_time();
    });

    // 4. Return result.
    return result;
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserverentrylist-getentries
WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> PerformanceObserverEntryList::get_entries() const
{
    // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
    // name and type set to null.
    return TRY_OR_THROW_OOM(vm(), filter_buffer_by_name_and_type(m_entry_list, /* name= */ Optional<String> {}, /* type= */ Optional<String> {}));
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserverentrylist-getentriesbytype
WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> PerformanceObserverEntryList::get_entries_by_type(String const& type) const
{
    // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
    // name set to null, and type set to the method's input type parameter.
    return TRY_OR_THROW_OOM(vm(), filter_buffer_by_name_and_type(m_entry_list, /* name= */ Optional<String> {}, type));
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserverentrylist-getentriesbyname
WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> PerformanceObserverEntryList::get_entries_by_name(String const& name, Optional<String> type) const
{
    // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
    // name set to the method input name parameter, and type set to null if optional entryType is omitted, or set to the
    // method's input type parameter otherwise.
    return TRY_OR_THROW_OOM(vm(), filter_buffer_by_name_and_type(m_entry_list, name, type));
}

}
