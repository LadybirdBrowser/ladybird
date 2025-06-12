/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::PerformanceTimeline::EventNames {

#define ENUMERATE_PERFORMANCE_TIMELINE_EVENTS \
    __ENUMERATE_PERFORMANCE_TIMELINE_EVENT(resourcetimingbufferfull)

#define __ENUMERATE_PERFORMANCE_TIMELINE_EVENT(name) extern FlyString name;
ENUMERATE_PERFORMANCE_TIMELINE_EVENTS
#undef __ENUMERATE_PERFORMANCE_TIMELINE_EVENT

}
