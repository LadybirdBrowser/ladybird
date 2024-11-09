/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/EventNames.h>

namespace Web::HTML::EventNames {

#define __ENUMERATE_HTML_EVENT(name) FlyString name;
ENUMERATE_HTML_EVENTS
#undef __ENUMERATE_HTML_EVENT

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_HTML_EVENT(name) \
    name = #name##_fly_string;
    ENUMERATE_HTML_EVENTS
#undef __ENUMERATE_HTML_EVENT

    s_initialized = true;
}

}
