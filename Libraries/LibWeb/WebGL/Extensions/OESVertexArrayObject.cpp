/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/Extensions/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/WebGLVertexArrayObjectOES.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(OESVertexArrayObject);

GC::Ref<WebGLExtension> OESVertexArrayObject::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = GC::Heap::the().allocate<OESVertexArrayObject>(context);
    return GC::Ref<WebGLExtension> { extension };
}

OESVertexArrayObject::OESVertexArrayObject(GC::Ref<WebGLRenderingContextBase> context)
    : m_context(context)
{
}

GC::Ref<WebGLVertexArrayObjectOES> OESVertexArrayObject::create_vertex_array_oes()
{
    m_context->context().make_current();

    GLuint handle = 0;
    glGenVertexArraysOES(1, &handle);
    return WebGLVertexArrayObjectOES::create(m_context, handle);
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

void OESVertexArrayObject::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
