/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
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

GC::Ref<WebGLUniformLocation> WebGLUniformLocation::create(JS::Realm& realm, GLuint handle, GC::Ptr<WebGLProgram> parent_shader)
{
    return realm.create<WebGLUniformLocation>(realm, handle, parent_shader);
}

WebGLUniformLocation::WebGLUniformLocation(JS::Realm& realm, GLuint handle, GC::Ptr<WebGLProgram> parent_shader)
    : Bindings::PlatformObject(realm)
    , m_handle(handle)
    , m_parent_shader(parent_shader)
{
}

WebGLUniformLocation::~WebGLUniformLocation() = default;

void WebGLUniformLocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLUniformLocation);
    Base::initialize(realm);
}

void WebGLUniformLocation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_shader);
}

ErrorOr<GLuint> WebGLUniformLocation::handle(GC::Ptr<WebGLProgram> current_shader) const
{
    if (current_shader == m_parent_shader)
        return m_handle;
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
