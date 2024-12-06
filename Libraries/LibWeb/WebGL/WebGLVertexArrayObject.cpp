/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLVertexArrayObjectPrototype.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLVertexArrayObject);

GC::Ref<WebGLVertexArrayObject> WebGLVertexArrayObject::create(JS::Realm& realm, GLuint handle)
{
    return realm.create<WebGLVertexArrayObject>(realm, handle);
}

WebGLVertexArrayObject::WebGLVertexArrayObject(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLVertexArrayObject::~WebGLVertexArrayObject() = default;

void WebGLVertexArrayObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLVertexArrayObject);
}

}
