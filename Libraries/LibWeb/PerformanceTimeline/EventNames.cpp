/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PerformanceTimeline/EventNames.h>

namespace Web::PerformanceTimeline::EventNames {

#define __ENUMERATE_PERFORMANCE_TIMELINE_EVENT(name) \
    Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_PERFORMANCE_TIMELINE_EVENTS
#undef __ENUMERATE_PERFORMANCE_TIMELINE_EVENT

}
