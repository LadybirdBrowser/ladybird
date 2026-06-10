/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLShader.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLShader);

GC::Ref<WebGLShader> WebGLShader::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle, GLenum type)
{
    return GC::Heap::the().allocate<WebGLShader>(context, handle, type);
}

WebGLShader::WebGLShader(GC::Ref<WebGLRenderingContextBase> context, GLuint handle, GLenum type)
    : WebGLObject(context, handle)
    , m_type(type)
{
}

WebGLShader::~WebGLShader() = default;

}
