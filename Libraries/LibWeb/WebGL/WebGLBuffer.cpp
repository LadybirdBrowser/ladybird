/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLBuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLBuffer);

GC::Ref<WebGLBuffer> WebGLBuffer::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLBuffer>(context, handle);
}

WebGLBuffer::WebGLBuffer(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLBuffer::~WebGLBuffer() = default;

bool WebGLBuffer::is_compatible_with(GLenum target)
{
    // https://registry.khronos.org/webgl/specs/latest/2.0/#5.1
    if (!m_target.has_value()) {
        m_target = target;
        return true;
    }

    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return m_target.value() == GL_ELEMENT_ARRAY_BUFFER;

    return m_target.value() != GL_ELEMENT_ARRAY_BUFFER;
}

}
