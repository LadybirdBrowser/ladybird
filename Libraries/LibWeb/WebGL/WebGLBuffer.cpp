/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLBufferPrototype.h>
#include <LibWeb/WebGL/WebGLBuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLBuffer);

GC::Ptr<WebGLBuffer> WebGLBuffer::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLBuffer>(realm, handle);
}

WebGLBuffer::WebGLBuffer(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLBuffer::~WebGLBuffer() = default;

}
