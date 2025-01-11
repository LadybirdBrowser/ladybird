/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/NavigationTiming/EntryNames.h>

namespace Web::NavigationTiming::EntryNames {

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) \
    FlyString name = #name##_fly_string;
ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

}
