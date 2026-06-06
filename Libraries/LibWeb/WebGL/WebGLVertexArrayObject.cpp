/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WebGLVertexArrayObject.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLVertexArrayObject);

GC::Ref<WebGLVertexArrayObject> WebGLVertexArrayObject::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLVertexArrayObject>(context, handle);
}

WebGLVertexArrayObject::WebGLVertexArrayObject(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLVertexArrayObject::~WebGLVertexArrayObject() = default;

}
