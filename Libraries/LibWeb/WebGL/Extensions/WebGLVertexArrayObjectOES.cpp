/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLVertexArrayObjectOES.h>
#include <LibWeb/WebGL/Extensions/WebGLVertexArrayObjectOES.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLVertexArrayObjectOES);

GC::Ref<WebGLVertexArrayObjectOES> WebGLVertexArrayObjectOES::create(WebGLRenderingContextBase& context, GLuint handle)
{
    auto& realm = context.realm();
    return realm.create<WebGLVertexArrayObjectOES>(realm, context, handle);
}

WebGLVertexArrayObjectOES::WebGLVertexArrayObjectOES(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLVertexArrayObjectOES::~WebGLVertexArrayObjectOES() = default;

}
