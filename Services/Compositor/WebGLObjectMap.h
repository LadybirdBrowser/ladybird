/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <LibCompositing/WebGL/GLFunctions.h>
#include <LibCompositing/WebGL/Types.h>

namespace Compositor {

class WebGLObjectMap {
public:
    GLuint lookup(Compositing::WebGL::WebGLObjectId) const;
    GLuint take(Compositing::WebGL::WebGLObjectId);
    ErrorOr<void> add(Compositing::WebGL::WebGLObjectId, GLuint);

    GLsync lookup_sync(Compositing::WebGL::WebGLObjectId) const;
    GLsync take_sync(Compositing::WebGL::WebGLObjectId);
    ErrorOr<void> add_sync(Compositing::WebGL::WebGLObjectId, GLsync);

private:
    HashMap<Compositing::WebGL::WebGLObjectId, GLuint> m_objects;
    HashMap<Compositing::WebGL::WebGLObjectId, GLsync> m_syncs;
};

}
