/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGL/EventNames.h>

namespace Web::WebGL::EventNames {

#define __ENUMERATE_GL_EVENT(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_GL_EVENTS
#undef __ENUMERATE_GL_EVENT

}
