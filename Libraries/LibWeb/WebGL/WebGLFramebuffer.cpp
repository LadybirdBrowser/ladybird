/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLFramebuffer);

GC::Ref<WebGLFramebuffer> WebGLFramebuffer::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLFramebuffer>(context, handle);
}

WebGLFramebuffer::WebGLFramebuffer(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLFramebuffer::~WebGLFramebuffer() = default;

}
