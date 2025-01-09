/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLShaderPrototype.h>
#include <LibWeb/WebGL/WebGLShader.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLShader);

GC::Ref<WebGLShader> WebGLShader::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle, GLenum type)
{
    return realm.create<WebGLShader>(realm, context, handle, type);
}

WebGLShader::WebGLShader(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle, GLenum type)
    : WebGLObject(realm, context, handle)
    , m_type(type)
{
}

WebGLShader::~WebGLShader() = default;

void WebGLShader::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLShader);
}

}
