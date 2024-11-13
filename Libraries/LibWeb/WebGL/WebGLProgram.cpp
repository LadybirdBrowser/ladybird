/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLProgramPrototype.h>
#include <LibWeb/WebGL/WebGLProgram.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLProgram);

WebGLProgram::WebGLProgram(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLProgram::~WebGLProgram() = default;

}
