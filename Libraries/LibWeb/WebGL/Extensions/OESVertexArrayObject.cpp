/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OESVertexArrayObjectPrototype.h>
#include <LibWeb/WebGL/Extensions/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/WebGLVertexArrayObjectOES.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(OESVertexArrayObject);

JS::ThrowCompletionOr<GC::Ptr<OESVertexArrayObject>> OESVertexArrayObject::create(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
{
    return realm.create<OESVertexArrayObject>(realm, context);
}

OESVertexArrayObject::OESVertexArrayObject(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_OES_vertex_array_object");
}

GC::Ref<WebGLVertexArrayObjectOES> OESVertexArrayObject::create_vertex_array_oes()
{
    m_context->context().make_current();

    GLuint handle = 0;
    glGenVertexArraysOES(1, &handle);
    return WebGLVertexArrayObjectOES::create(realm(), m_context, handle);
}

void OESVertexArrayObject::delete_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object)
{
    m_context->context().make_current();

    GLuint vertex_array_handle = 0;
    if (array_object) {
        auto handle_or_error = array_object->handle(m_context.ptr());
        if (handle_or_error.is_error()) {
            // FIXME: m_context->set_error(GL_INVALID_OPERATION);
            return;
        }
        vertex_array_handle = handle_or_error.release_value();
    }

    glDeleteVertexArraysOES(1, &vertex_array_handle);
}

bool OESVertexArrayObject::is_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object)
{
    m_context->context().make_current();

    GLuint vertex_array_handle = 0;
    if (array_object) {
        auto handle_or_error = array_object->handle(m_context.ptr());
        if (handle_or_error.is_error()) {
            return false;
        }
        vertex_array_handle = handle_or_error.release_value();
    }

    return glIsVertexArrayOES(vertex_array_handle) == GL_TRUE;
}

void OESVertexArrayObject::bind_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object)
{
    m_context->context().make_current();

    GLuint vertex_array_handle = 0;
    if (array_object) {
        auto handle_or_error = array_object->handle(m_context.ptr());
        if (handle_or_error.is_error()) {
            // FIXME: m_context->set_error(GL_INVALID_OPERATION);
            return;
        }
        vertex_array_handle = handle_or_error.release_value();
    }

    glBindVertexArrayOES(vertex_array_handle);
}

void OESVertexArrayObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OESVertexArrayObject);
}

void OESVertexArrayObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
