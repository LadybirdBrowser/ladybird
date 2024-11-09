/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::PerformanceTimeline::EntryTypes {

// https://w3c.github.io/timing-entrytypes-registry/#registry
#define ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPES                        \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(element)                  \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(event)                    \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(first_input)              \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(largest_contentful_paint) \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(layout_shift)             \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(longtask)                 \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(mark)                     \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(measure)                  \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(navigation)               \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(resource)                 \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(paint)

#define __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(name) extern FlyString name;
ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPES
#undef __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE

void initialize_strings();

}
