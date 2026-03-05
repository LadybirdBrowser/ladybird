/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLCompressedTextureS3tcSrgbPrototype.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tcSrgb.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(WebGLCompressedTextureS3tcSrgb);

JS::ThrowCompletionOr<GC::Ref<JS::Object>> WebGLCompressedTextureS3tcSrgb::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    return realm.create<WebGLCompressedTextureS3tcSrgb>(realm, context);
}

WebGLCompressedTextureS3tcSrgb::WebGLCompressedTextureS3tcSrgb(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->enable_compressed_texture_format(GL_COMPRESSED_SRGB_S3TC_DXT1_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT);
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
