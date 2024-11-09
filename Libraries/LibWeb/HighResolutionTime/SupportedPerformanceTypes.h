/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HighResolutionTime {

// Please keep these in alphabetical order based on the entry type :^)
#define ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES                                                                   \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::mark, UserTiming::PerformanceMark) \
    __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(PerformanceTimeline::EntryTypes::measure, UserTiming::PerformanceMeasure)

}
