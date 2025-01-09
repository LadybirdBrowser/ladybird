/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLVertexArrayObjectOESPrototype.h>
#include <LibWeb/WebGL/WebGLVertexArrayObjectOES.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLVertexArrayObjectOES);

GC::Ref<WebGLVertexArrayObjectOES> WebGLVertexArrayObjectOES::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLVertexArrayObjectOES>(realm, context, handle);
}

WebGLVertexArrayObjectOES::WebGLVertexArrayObjectOES(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLVertexArrayObjectOES::~WebGLVertexArrayObjectOES() = default;

void WebGLVertexArrayObjectOES::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLVertexArrayObjectOES);
}

}
