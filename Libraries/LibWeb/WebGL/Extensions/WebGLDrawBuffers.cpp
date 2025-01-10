/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLDrawBuffersPrototype.h>
#include <LibWeb/WebGL/Extensions/WebGLDrawBuffers.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(WebGLDrawBuffers);

JS::ThrowCompletionOr<GC::Ptr<WebGLDrawBuffers>> WebGLDrawBuffers::create(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
{
    return realm.create<WebGLDrawBuffers>(realm, context);
}

WebGLDrawBuffers::WebGLDrawBuffers(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_draw_buffers");
}

void WebGLDrawBuffers::draw_buffers_webgl(Vector<GLenum> buffers)
{
    m_context->context().make_current();
    glDrawBuffersEXT(buffers.size(), buffers.data());
}

void WebGLDrawBuffers::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLDrawBuffers);
}

void WebGLDrawBuffers::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
