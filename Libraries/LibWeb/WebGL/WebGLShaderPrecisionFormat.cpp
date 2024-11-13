/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLShaderPrecisionFormatPrototype.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLShaderPrecisionFormat);

WebGLShaderPrecisionFormat::WebGLShaderPrecisionFormat(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLShaderPrecisionFormat::~WebGLShaderPrecisionFormat() = default;

}
