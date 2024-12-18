/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLSamplerPrototype.h>
#include <LibWeb/WebGL/WebGLSampler.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLSampler);

GC::Ref<WebGLSampler> WebGLSampler::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLSampler>(realm, context, handle);
}

WebGLSampler::WebGLSampler(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLSampler::~WebGLSampler() = default;

void WebGLSampler::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLSampler);
}

}
