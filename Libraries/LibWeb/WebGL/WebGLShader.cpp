/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLShaderPrototype.h>
#include <LibWeb/WebGL/WebGLShader.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLShader);

GC::Ptr<WebGLShader> WebGLShader::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLShader>(realm, handle);
}

WebGLShader::WebGLShader(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLShader::~WebGLShader() = default;

}
