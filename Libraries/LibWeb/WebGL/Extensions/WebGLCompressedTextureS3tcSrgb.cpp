/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLCompressedTextureS3tcSrgbPrototype.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tcSrgb.h>
#include <LibWeb/WebGL/OpenGLContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(WebGLCompressedTextureS3tcSrgb);

JS::ThrowCompletionOr<GC::Ptr<WebGLCompressedTextureS3tcSrgb>> WebGLCompressedTextureS3tcSrgb::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    return realm.create<WebGLCompressedTextureS3tcSrgb>(realm, context);
}

WebGLCompressedTextureS3tcSrgb::WebGLCompressedTextureS3tcSrgb(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_texture_compression_s3tc_srgb");
}

void WebGLCompressedTextureS3tcSrgb::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLCompressedTextureS3tcSrgb);
    Base::initialize(realm);
}

void WebGLCompressedTextureS3tcSrgb::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
