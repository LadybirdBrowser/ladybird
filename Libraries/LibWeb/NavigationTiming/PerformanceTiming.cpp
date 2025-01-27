/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PerformanceTimingPrototype.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/PerformanceTiming.h>

namespace Web::NavigationTiming {
GC_DEFINE_ALLOCATOR(PerformanceTiming);

PerformanceTiming::PerformanceTiming(JS::Realm& realm)
    : PlatformObject(realm)
{
}

PerformanceTiming::~PerformanceTiming() = default;

void PerformanceTiming::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceTiming);
}

DOM::DocumentLoadTimingInfo const& PerformanceTiming::document_load_timing_info(JS::Object const& global_object) const
{
    VERIFY(is<HTML::Window>(global_object));
    auto& window = static_cast<HTML::Window const&>(global_object);
    auto document = window.document();
    return document->load_timing_info();
}

u64 PerformanceTiming::monotonic_timestamp_to_wall_time_milliseconds(Function<HighResolutionTime::DOMHighResTimeStamp(DOM::DocumentLoadTimingInfo const&)> selector) const
{
    auto& global_object = HTML::relevant_global_object(*this);
    auto timestamp = selector(document_load_timing_info(global_object));
    if (timestamp == 0)
        return 0;

    auto wall_time = timestamp - HighResolutionTime::estimated_monotonic_time_of_the_unix_epoch();
    auto coarsened_time = HighResolutionTime::coarsen_time(wall_time);
    return static_cast<u64>(coarsened_time);
}

}
