/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLObject.h>
#include <LibWeb/WebGL/WebGLObject.h>

#include <GLES2/gl2.h>

namespace Web::WebGL {

WebGLObject::WebGLObject(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : Bindings::PlatformObject(realm)
    , m_context(context)
    , m_handle(handle)
    , m_context_generation(context->context_generation())
{
}

WebGLObject::~WebGLObject() = default;

void WebGLObject::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLObject);
    Base::initialize(realm);
}

void WebGLObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

ErrorOr<GLuint> WebGLObject::handle(WebGLRenderingContextBase const* context) const
{
    TRY(validate_context(context));
    if (invalidated_for_context(context))
        return Error::from_errno(GL_INVALID_OPERATION);
    return m_handle;
}

ErrorOr<Optional<GLuint>> WebGLObject::handle_for_deletion(WebGLRenderingContextBase const* context)
{
    TRY(validate_context(context));
    if (invalidated_for_context(context))
        return Optional<GLuint> {};
    m_invalidated = true;
    return Optional<GLuint> { m_handle };
}

ErrorOr<Optional<GLuint>> WebGLObject::handle_for_query(WebGLRenderingContextBase const* context) const
{
    TRY(validate_context(context));
    if (invalidated_for_context(context))
        return Optional<GLuint> {};
    return Optional<GLuint> { m_handle };
}

bool WebGLObject::invalidated_for_context(WebGLRenderingContextBase const* context) const
{
    return m_invalidated || m_context_generation != context->context_generation();
}

ErrorOr<void> WebGLObject::validate_context(WebGLRenderingContextBase const* context) const
{
    if (context == m_context)
        return {};
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
