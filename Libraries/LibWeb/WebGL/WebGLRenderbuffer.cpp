/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLRenderbuffer);

GC::Ref<WebGLRenderbuffer> WebGLRenderbuffer::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLRenderbuffer>(context, handle);
}

WebGLRenderbuffer::WebGLRenderbuffer(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLRenderbuffer::~WebGLRenderbuffer() = default;

}
