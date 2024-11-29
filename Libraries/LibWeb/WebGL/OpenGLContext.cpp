/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebGL/OpenGLContext.h>

namespace Web::WebGL {

OwnPtr<OpenGLContext> OpenGLContext::create()
{
    return {};
}

void OpenGLContext::clear_buffer_to_default_values()
{
}

}
