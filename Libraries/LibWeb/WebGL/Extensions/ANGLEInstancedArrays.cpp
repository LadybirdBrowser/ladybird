/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/WebGL/Extensions/ANGLEInstancedArrays.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(ANGLEInstancedArrays);

JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> ANGLEInstancedArrays::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = realm.create<ANGLEInstancedArrays>(realm, context);
    return GC::Ref<Bindings::Wrappable> { extension };
}

ANGLEInstancedArrays::ANGLEInstancedArrays(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : Wrappable(realm)
    , m_context(context)
{
}

void ANGLEInstancedArrays::vertex_attrib_divisor_angle(GLuint index, GLuint divisor)
{
    m_context->context().make_current();
    glVertexAttribDivisorANGLE(index, divisor);
}

void ANGLEInstancedArrays::draw_arrays_instanced_angle(GLenum mode, GLint first, GLsizei count, GLsizei primcount)
{
    m_context->context().make_current();
    glDrawArraysInstancedANGLE(mode, first, count, primcount);
}

void ANGLEInstancedArrays::draw_elements_instanced_angle(GLenum mode, GLsizei count, GLenum type, GLintptr offset, GLsizei primcount)
{
    m_context->context().make_current();
    glDrawElementsInstancedANGLE(mode, count, type, reinterpret_cast<void*>(offset), primcount);
}

void ANGLEInstancedArrays::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
