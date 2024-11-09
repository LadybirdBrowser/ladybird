/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/XHR/EventNames.h>

namespace Web::XHR::EventNames {

#define __ENUMERATE_XHR_EVENT(name) FlyString name;
ENUMERATE_XHR_EVENTS
#undef __ENUMERATE_XHR_EVENT

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_XHR_EVENT(name) \
    name = #name##_fly_string;
    ENUMERATE_XHR_EVENTS
#undef __ENUMERATE_XHR_EVENT

    s_initialized = true;
}

}
