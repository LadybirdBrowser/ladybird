/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FlyString.h>

namespace Web::WebGL::EventNames {

#define ENUMERATE_GL_EVENTS                         \
    __ENUMERATE_GL_EVENT(webglcontextcreationerror) \
    __ENUMERATE_GL_EVENT(webglcontextlost)          \
    __ENUMERATE_GL_EVENT(webglcontextrestored)

#define __ENUMERATE_GL_EVENT(name) extern FlyString name;
ENUMERATE_GL_EVENTS
#undef __ENUMERATE_GL_EVENT

void initialize_strings();

}
