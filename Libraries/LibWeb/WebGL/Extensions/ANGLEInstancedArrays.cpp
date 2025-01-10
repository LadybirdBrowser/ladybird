/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/ANGLEInstancedArraysPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/ANGLEInstancedArrays.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(ANGLEInstancedArrays);

JS::ThrowCompletionOr<GC::Ptr<ANGLEInstancedArrays>> ANGLEInstancedArrays::create(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
{
    return realm.create<ANGLEInstancedArrays>(realm, context);
}

ANGLEInstancedArrays::ANGLEInstancedArrays(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_ANGLE_instanced_arrays");
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

void ANGLEInstancedArrays::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ANGLEInstancedArrays);
}

void ANGLEInstancedArrays::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
