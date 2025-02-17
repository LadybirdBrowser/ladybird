/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TimeRangesPrototype.h>
#include <LibWeb/HTML/TimeRanges.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TimeRanges);

TimeRanges::TimeRanges(JS::Realm& realm)
    : Base(realm)
{
}

void TimeRanges::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TimeRanges);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-length
size_t TimeRanges::length() const
{
    return m_ranges.size();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-start
WebIDL::ExceptionOr<double> TimeRanges::start(u32 index) const
{
    // These methods must throw "IndexSizeError" DOMExceptions if called with an index argument greater than or equal to the number of ranges represented by the object.
    if (index >= m_ranges.size())
        return WebIDL::IndexSizeError::create(realm(), "Index argument is greater than or equal to the number of ranges represented by this TimeRanges object"_string);

    // The start(index) method must return the position of the start of the indexth range represented by the object,
    // in seconds measured from the start of the timeline that the object covers.
    return m_ranges[index].start;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-end
WebIDL::ExceptionOr<double> TimeRanges::end(u32 index) const
{
    // These methods must throw "IndexSizeError" DOMExceptions if called with an index argument greater than or equal to the number of ranges represented by the object.
    if (index >= m_ranges.size())
        return WebIDL::IndexSizeError::create(realm(), "Index argument is greater than or equal to the number of ranges represented by this TimeRanges object"_string);

    // The end(index) method must return the position of the end of the indexth range represented by the object,
    // in seconds measured from the start of the timeline that the object covers.
    return m_ranges[index].end;
}

void TimeRanges::add_range(double start, double end)
{
    m_ranges.append({ start, end });
}
bool TimeRanges::in_range(double point)
{
    for (auto range : m_ranges) {
        if (point >= range.start && point <= range.end)
            return true;
    }
    return false;
}

}
