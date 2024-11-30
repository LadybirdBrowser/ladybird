/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLUniformLocationPrototype.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLUniformLocation);

GC::Ptr<WebGLUniformLocation> WebGLUniformLocation::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLUniformLocation>(realm, handle);
}

WebGLUniformLocation::WebGLUniformLocation(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLUniformLocation::~WebGLUniformLocation() = default;

}
