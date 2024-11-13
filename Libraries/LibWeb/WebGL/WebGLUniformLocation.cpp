/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLUniformLocationPrototype.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLUniformLocation);

WebGLUniformLocation::WebGLUniformLocation(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLUniformLocation::~WebGLUniformLocation() = default;

}
