/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLSampler.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLSampler);

GC::Ref<WebGLSampler> WebGLSampler::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLSampler>(context, handle);
}

WebGLSampler::WebGLSampler(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLSampler::~WebGLSampler() = default;

}
