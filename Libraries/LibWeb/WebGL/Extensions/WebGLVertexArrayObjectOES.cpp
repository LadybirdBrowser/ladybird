/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/Extensions/WebGLVertexArrayObjectOES.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLVertexArrayObjectOES);

GC::Ref<WebGLVertexArrayObjectOES> WebGLVertexArrayObjectOES::create(WebGLRenderingContextBase& context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLVertexArrayObjectOES>(context, handle);
}

WebGLVertexArrayObjectOES::WebGLVertexArrayObjectOES(WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLVertexArrayObjectOES::~WebGLVertexArrayObjectOES() = default;

}
