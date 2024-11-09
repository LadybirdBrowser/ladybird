/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/UIEvents/EventNames.h>

namespace Web::UIEvents::EventNames {

#define __ENUMERATE_UI_EVENT(name) FlyString name;
ENUMERATE_UI_EVENTS
#undef __ENUMERATE_UI_EVENT

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_UI_EVENT(name) \
    name = #name##_fly_string;
    ENUMERATE_UI_EVENTS
#undef __ENUMERATE_UI_EVENT

    s_initialized = true;
}

}
