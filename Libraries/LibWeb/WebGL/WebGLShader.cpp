/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLShaderPrototype.h>
#include <LibWeb/WebGL/WebGLShader.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLShader);

WebGLShader::WebGLShader(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLShader::~WebGLShader() = default;

}
