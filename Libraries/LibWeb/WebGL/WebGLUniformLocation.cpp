/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>

#include <GLES2/gl2.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLUniformLocation);

GC::Ref<WebGLUniformLocation> WebGLUniformLocation::create(GLuint handle, GC::Ptr<WebGLProgram> parent_shader)
{
    return GC::Heap::the().allocate<WebGLUniformLocation>(handle, parent_shader);
}

WebGLUniformLocation::WebGLUniformLocation(GLuint handle, GC::Ptr<WebGLProgram> parent_shader)
    : m_handle(handle)
    , m_parent_shader(parent_shader)
{
}

WebGLUniformLocation::~WebGLUniformLocation() = default;

void WebGLUniformLocation::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_shader);
}

ErrorOr<GLuint> WebGLUniformLocation::handle(GC::Ptr<WebGLProgram> current_shader) const
{
    if (current_shader == m_parent_shader)
        return m_handle;
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
