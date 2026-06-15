/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/WebGLVertexArrayObjectOES.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(OESVertexArrayObject);

JS::ThrowCompletionOr<GC::Ref<JS::Object>> OESVertexArrayObject::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    return realm.create<OESVertexArrayObject>(realm, context);
}

OESVertexArrayObject::OESVertexArrayObject(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : PlatformObject(realm)
    , m_context(context)
{
}

GC::Ref<WebGLVertexArrayObjectOES> OESVertexArrayObject::create_vertex_array_oes()
{
    m_context->context().make_current();

    GLuint handle = 0;
    m_context->context().gen_vertex_arrays_oes(1, &handle);
    return WebGLVertexArrayObjectOES::create(realm(), m_context, handle);
}

void OESVertexArrayObject::delete_vertex_array_oes(GC::Ptr<WebGLVertexArrayObjectOES> array_object)
{
    m_context->context().make_current();

    if (!array_object)
        return;

    auto handle_or_error = array_object->handle_for_deletion(m_context.ptr());
    if (handle_or_error.is_error()) {
        // FIXME: m_context->set_error(GL_INVALID_OPERATION);
        return;
    }
    auto vertex_array_handle = handle_or_error.release_value();
    if (!vertex_array_handle.has_value())
        return;

    auto handle = vertex_array_handle.value();
    m_context->context().delete_vertex_arrays_oes(1, &handle);
}

bool OESVertexArrayObject::is_vertex_array_oes(GC::Ptr<WebGLVertexArrayObjectOES> array_object)
{
    m_context->context().make_current();

    if (!array_object)
        return false;

    auto handle_or_error = array_object->handle_for_query(m_context.ptr());
    if (handle_or_error.is_error()) {
        return false;
    }
    auto vertex_array_handle = handle_or_error.release_value();
    if (!vertex_array_handle.has_value())
        return false;

    return m_context->context().is_vertex_array_oes(vertex_array_handle.value()) == GL_TRUE;
}

void OESVertexArrayObject::bind_vertex_array_oes(GC::Ptr<WebGLVertexArrayObjectOES> array_object)
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

    m_context->context().bind_vertex_array_oes(vertex_array_handle);
}

void OESVertexArrayObject::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OESVertexArrayObject);
    Base::initialize(realm);
}

void OESVertexArrayObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
