/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGL/EventNames.h>

namespace Web::WebGL::EventNames {

#define __ENUMERATE_GL_EVENT(name) FlyString name;
ENUMERATE_GL_EVENTS
#undef __ENUMERATE_GL_EVENT

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_GL_EVENT(name) \
    name = #name##_fly_string;
    ENUMERATE_GL_EVENTS
#undef __ENUMERATE_GL_EVENT

    s_initialized = true;
}

}
