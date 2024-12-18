/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLTransformFeedbackPrototype.h>
#include <LibWeb/WebGL/WebGLTransformFeedback.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLTransformFeedback);

GC::Ref<WebGLTransformFeedback> WebGLTransformFeedback::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLTransformFeedback>(realm, context, handle);
}

WebGLTransformFeedback::WebGLTransformFeedback(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLTransformFeedback::~WebGLTransformFeedback() = default;

void WebGLTransformFeedback::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLTransformFeedback);
}

}
