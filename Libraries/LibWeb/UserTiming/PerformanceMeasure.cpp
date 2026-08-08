/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/UserTiming/PerformanceMeasure.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UserTiming {

GC_DEFINE_ALLOCATOR(PerformanceMeasure);

PerformanceMeasure::PerformanceMeasure(Utf16String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail)
    : PerformanceTimeline::PerformanceEntry(name, start_time, duration)
    , m_detail(detail)
{
}

PerformanceMeasure::~PerformanceMeasure() = default;

GC::Ref<PerformanceMeasure> PerformanceMeasure::create(Utf16String const& measure_name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail)
{
    return GC::Heap::the().allocate<PerformanceMeasure>(measure_name, start_time, duration, move(detail));
}

Utf16FlyString const& PerformanceMeasure::entry_type() const
{
    return PerformanceTimeline::EntryTypes::measure;
}

WebIDL::ExceptionOr<JS::Value> PerformanceMeasure::detail(JS::Object const& relevant_global_object) const
{
    auto& relevant_realm = HTML::relevant_realm(relevant_global_object);
    auto serialized = TRY(HTML::structured_serialize(relevant_realm.vm(), m_detail));
    return HTML::structured_deserialize(relevant_realm.vm(), serialized, relevant_realm);
}

void PerformanceMeasure::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_detail);
}

}
