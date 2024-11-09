/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/NavigationTiming/EntryNames.h>

namespace Web::NavigationTiming::EntryNames {

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) FlyString name;
ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) \
    name = #name##_fly_string;
    ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

    s_initialized = true;
}

}
