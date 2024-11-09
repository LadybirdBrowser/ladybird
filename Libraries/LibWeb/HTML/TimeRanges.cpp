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

JS_DEFINE_ALLOCATOR(TimeRanges);

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
    // FIXME: The length IDL attribute must return the number of ranges represented by the object.
    return 0;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-start
double TimeRanges::start(u32) const
{
    // FIXME: The start(index) method must return the position of the start of the indexth range represented by the object,
    //        in seconds measured from the start of the timeline that the object covers.
    return 0.0;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-end
double TimeRanges::end(u32) const
{
    // FIXME: The end(index) method must return the position of the end of the indexth range represented by the object,
    //        in seconds measured from the start of the timeline that the object covers.
    return 0.0;
}

}
