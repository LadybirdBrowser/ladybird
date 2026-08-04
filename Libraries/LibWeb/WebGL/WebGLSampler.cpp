/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLSampler.h>
#include <LibWeb/WebGL/WebGLSampler.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLSampler);

GC::Ref<WebGLSampler> WebGLSampler::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    auto& realm = context->realm();
    return realm.create<WebGLSampler>(realm, context, handle);
}

WebGLSampler::WebGLSampler(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLSampler::~WebGLSampler() = default;

}
