/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HighResolutionTime {

// The global objects an entry type is supported for.
enum class SupportedPerformanceEntryTypeGlobals {
    WindowAndWorker,
    WindowOnly,
};

inline bool is_supported_for_global(SupportedPerformanceEntryTypeGlobals globals, bool global_is_window)
{
    return globals == SupportedPerformanceEntryTypeGlobals::WindowAndWorker || global_is_window;
}

// Please keep these in alphabetical order based on the entry type :^)
#define ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES                                                                                                                      \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::event, EventTiming::EventEntryTypeRegistry, WindowOnly)                               \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::first_input, EventTiming::FirstInputEntryTypeRegistry, WindowOnly)                    \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::largest_contentful_paint, LargestContentfulPaint::LargestContentfulPaint, WindowOnly) \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::mark, UserTiming::PerformanceMark, WindowAndWorker)                                   \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::measure, UserTiming::PerformanceMeasure, WindowAndWorker)                             \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::navigation, NavigationTiming::PerformanceNavigationTiming, WindowOnly)                \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::paint, PaintTiming::PerformancePaintTiming, WindowOnly)                               \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::resource, ResourceTiming::PerformanceResourceTiming, WindowAndWorker)

}
