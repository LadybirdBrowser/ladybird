/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLProgramPrototype.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLShader.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLProgram);

GC::Ref<WebGLProgram> WebGLProgram::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLProgram>(realm, context, handle);
}

WebGLProgram::WebGLProgram(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLProgram::~WebGLProgram() = default;

void WebGLProgram::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLProgram);
}

void WebGLProgram::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_attached_vertex_shader);
    visitor.visit(m_attached_fragment_shader);
}

}
