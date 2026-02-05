/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::PerformanceTimeline::EntryTypes {

// https://w3c.github.io/timing-entrytypes-registry/#registry
#define ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPES                                                    \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(element, "element")                                   \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(event, "event")                                       \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(first_input, "first-input")                           \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(largest_contentful_paint, "largest-contentful-paint") \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(layout_shift, "layout-shift")                         \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(longtask, "longtask")                                 \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(mark, "mark")                                         \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(measure, "measure")                                   \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(navigation, "navigation")                             \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(paint, "paint")                                       \
    __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(resource, "resource")

#define __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(name, type) extern FlyString name;
ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPES
#undef __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE

}
