/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/WebGL/Extensions/WebGLDrawBuffers.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLDrawBuffers);

JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> WebGLDrawBuffers::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = realm.create<WebGLDrawBuffers>(realm, context);
    return GC::Ref<Bindings::Wrappable> { extension };
}

WebGLDrawBuffers::WebGLDrawBuffers(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : Wrappable(realm)
    , m_context(context)
{
}

void WebGLDrawBuffers::draw_buffers_webgl(Vector<GLenum> buffers)
{
    m_context->context().make_current();
    glDrawBuffersEXT(buffers.size(), buffers.data());
}

void WebGLDrawBuffers::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
