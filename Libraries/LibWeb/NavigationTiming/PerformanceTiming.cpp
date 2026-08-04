/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/PerformanceTiming.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceTiming);

GC::Ref<PerformanceTiming> PerformanceTiming::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<PerformanceTiming>(window);
}

PerformanceTiming::PerformanceTiming(HTML::Window& window)
    : m_window(window)
{
}

PerformanceTiming::~PerformanceTiming() = default;

void PerformanceTiming::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

DOM::DocumentLoadTimingInfo const& PerformanceTiming::document_load_timing_info() const
{
    auto document = m_window->document();
    return document->load_timing_info();
}

u64 PerformanceTiming::monotonic_timestamp_to_wall_time_milliseconds(Function<HighResolutionTime::DOMHighResTimeStamp(DOM::DocumentLoadTimingInfo const&)> selector) const
{
    auto timestamp = selector(document_load_timing_info());
    if (timestamp == 0)
        return 0;

    auto wall_time = timestamp - HighResolutionTime::estimated_monotonic_time_of_the_unix_epoch();
    auto coarsened_time = HighResolutionTime::coarsen_time(wall_time);
    return static_cast<u64>(coarsened_time);
}

u64 PerformanceTiming::relative_timestamp_to_wall_time_milliseconds(Function<HighResolutionTime::DOMHighResTimeStamp(DOM::DocumentLoadTimingInfo const&)> selector) const
{
    auto const& load_info = document_load_timing_info();
    auto relative_timestamp = selector(load_info);
    if (relative_timestamp == 0)
        return 0;

    auto absolute_timestamp = relative_timestamp + load_info.navigation_start_time;
    auto wall_time = absolute_timestamp - HighResolutionTime::estimated_monotonic_time_of_the_unix_epoch();
    auto coarsened_time = HighResolutionTime::coarsen_time(wall_time);
    return static_cast<u64>(coarsened_time);
}

}
