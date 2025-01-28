/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLCompressedTextureS3tcPrototype.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
#include <LibWeb/WebGL/OpenGLContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(WebGLCompressedTextureS3tc);

JS::ThrowCompletionOr<GC::Ptr<WebGLCompressedTextureS3tc>> WebGLCompressedTextureS3tc::create(JS::Realm& realm, WebGLRenderingContextBase* context)
{
    return realm.create<WebGLCompressedTextureS3tc>(realm, context);
}

WebGLCompressedTextureS3tc::WebGLCompressedTextureS3tc(JS::Realm& realm, WebGLRenderingContextBase* context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_texture_compression_dxt1");
    m_context->context().request_extension("GL_ANGLE_texture_compression_dxt3");
    m_context->context().request_extension("GL_ANGLE_texture_compression_dxt5");
}

void WebGLCompressedTextureS3tc::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLCompressedTextureS3tc);
}

void WebGLCompressedTextureS3tc::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context->gc_cell());
}

}
