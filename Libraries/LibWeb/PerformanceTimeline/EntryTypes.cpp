/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::PerformanceTimeline::EntryTypes {

#define __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE(name, type) \
    FlyString name = type##_fly_string;
ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPES
#undef __ENUMERATE_PERFORMANCE_TIMELINE_ENTRY_TYPE

}
