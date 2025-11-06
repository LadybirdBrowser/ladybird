/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLBufferPrototype.h>
#include <LibWeb/WebGL/WebGLBuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLBuffer);

GC::Ref<WebGLBuffer> WebGLBuffer::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLBuffer>(realm, context, handle);
}

WebGLBuffer::WebGLBuffer(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLBuffer::~WebGLBuffer() = default;

void WebGLBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLBuffer);
    Base::initialize(realm);
}

bool WebGLBuffer::is_compatible_with(GLenum target)
{
    // https://registry.khronos.org/webgl/specs/latest/2.0/#5.1
    if (!m_target.has_value()) {
        m_target = target;
        return true;
    }

    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return m_target.value() == GL_ELEMENT_ARRAY_BUFFER;

    return m_target.value() != GL_ELEMENT_ARRAY_BUFFER;
}

}
