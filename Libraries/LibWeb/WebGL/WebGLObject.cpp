/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLObjectPrototype.h>
#include <LibWeb/WebGL/WebGLObject.h>

#include <GLES2/gl2.h>

namespace Web::WebGL {

WebGLObject::WebGLObject(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : Bindings::PlatformObject(realm)
    , m_context(&context)
    , m_handle(handle)
{
}

WebGLObject::~WebGLObject() = default;

void WebGLObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLObject);
}

void WebGLObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context->gc_cell());
}

ErrorOr<GLuint> WebGLObject::handle(WebGLRenderingContextBase const* context) const
{
    if (context == m_context)
        return m_handle;
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
