/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLTexture.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLTexture);

GC::Ref<WebGLTexture> WebGLTexture::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLTexture>(context, handle);
}

WebGLTexture::WebGLTexture(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLTexture::~WebGLTexture() = default;

}
