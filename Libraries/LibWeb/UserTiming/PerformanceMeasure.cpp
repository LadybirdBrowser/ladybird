/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/UserTiming/PerformanceMeasure.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UserTiming {

GC_DEFINE_ALLOCATOR(PerformanceMeasure);

PerformanceMeasure::PerformanceMeasure(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail)
    : PerformanceTimeline::PerformanceEntry(name, start_time, duration)
    , m_detail(move(detail))
{
}

PerformanceMeasure::~PerformanceMeasure() = default;

GC::Ref<PerformanceMeasure> PerformanceMeasure::create(String const& measure_name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, Optional<HTML::SerializationRecord> detail)
{
    return GC::Heap::the().allocate<PerformanceMeasure>(measure_name, start_time, duration, move(detail));
}

FlyString const& PerformanceMeasure::entry_type() const
{
    return PerformanceTimeline::EntryTypes::measure;
}

WebIDL::ExceptionOr<JS::Value> PerformanceMeasure::detail(JS::Realm& realm) const
{
    if (!m_detail.has_value())
        return JS::js_null();

    return HTML::structured_deserialize(realm.vm(), m_detail.value(), realm);
}

}
