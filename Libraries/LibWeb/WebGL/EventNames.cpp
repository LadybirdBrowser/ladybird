/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGL/EventNames.h>

namespace Web::WebGL::EventNames {

#define __ENUMERATE_GL_EVENT(name) \
    Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_GL_EVENTS
#undef __ENUMERATE_GL_EVENT

}
