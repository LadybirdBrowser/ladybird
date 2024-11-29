/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class OpenGLContext {
public:
    static OwnPtr<OpenGLContext> create();

    virtual void present() = 0;
    void clear_buffer_to_default_values();

    virtual ~OpenGLContext() { }
};

}
