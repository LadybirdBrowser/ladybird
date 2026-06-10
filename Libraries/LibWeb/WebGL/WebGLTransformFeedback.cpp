/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLTransformFeedback.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLTransformFeedback);

GC::Ref<WebGLTransformFeedback> WebGLTransformFeedback::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLTransformFeedback>(context, handle);
}

WebGLTransformFeedback::WebGLTransformFeedback(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLTransformFeedback::~WebGLTransformFeedback() = default;

}
