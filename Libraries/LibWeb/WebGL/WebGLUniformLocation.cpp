/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLUniformLocationPrototype.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>

#include <GLES2/gl2.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLUniformLocation);

GC::Ref<WebGLUniformLocation> WebGLUniformLocation::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLUniformLocation>(realm, context, handle);
}

WebGLUniformLocation::WebGLUniformLocation(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : Bindings::PlatformObject(realm)
    , m_context(&context)
    , m_handle(handle)
{
}

WebGLUniformLocation::~WebGLUniformLocation() = default;

void WebGLUniformLocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLUniformLocation);
    Base::initialize(realm);
}

void WebGLUniformLocation::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context->gc_cell());
}

ErrorOr<GLint> WebGLUniformLocation::handle(WebGLRenderingContextBase const* context) const
{
    if (context == m_context) [[likely]]
        return m_handle;
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
