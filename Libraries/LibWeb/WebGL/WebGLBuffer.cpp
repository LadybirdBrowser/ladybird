/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLBufferPrototype.h>
#include <LibWeb/WebGL/WebGLBuffer.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLBuffer);

WebGLBuffer::WebGLBuffer(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLBuffer::~WebGLBuffer() = default;

}
