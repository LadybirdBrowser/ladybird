/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLShader.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLProgram);

GC::Ref<WebGLProgram> WebGLProgram::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLProgram>(context, handle);
}

WebGLProgram::WebGLProgram(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLProgram::~WebGLProgram() = default;

void WebGLProgram::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_attached_vertex_shader);
    visitor.visit(m_attached_fragment_shader);
}

}
