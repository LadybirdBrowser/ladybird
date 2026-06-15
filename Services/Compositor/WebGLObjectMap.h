/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <LibWeb/WebGL/GLFunctions.h>
#include <LibWeb/WebGL/Types.h>

namespace Compositor {

class WebGLObjectMap {
public:
    GLuint lookup(Web::WebGL::WebGLObjectId) const;
    GLuint take(Web::WebGL::WebGLObjectId);
    ErrorOr<void> add(Web::WebGL::WebGLObjectId, GLuint);

    GLsync lookup_sync(Web::WebGL::WebGLObjectId) const;
    GLsync take_sync(Web::WebGL::WebGLObjectId);
    ErrorOr<void> add_sync(Web::WebGL::WebGLObjectId, GLsync);

private:
    HashMap<Web::WebGL::WebGLObjectId, GLuint> m_objects;
    HashMap<Web::WebGL::WebGLObjectId, GLsync> m_syncs;
};

}
