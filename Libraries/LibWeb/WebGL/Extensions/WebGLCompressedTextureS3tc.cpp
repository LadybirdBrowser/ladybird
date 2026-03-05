/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLCompressedTextureS3tcPrototype.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(WebGLCompressedTextureS3tc);

JS::ThrowCompletionOr<GC::Ref<JS::Object>> WebGLCompressedTextureS3tc::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    return realm.create<WebGLCompressedTextureS3tc>(realm, context);
}

WebGLCompressedTextureS3tc::WebGLCompressedTextureS3tc(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
}

void WebGLCompressedTextureS3tc::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLCompressedTextureS3tc);
    Base::initialize(realm);
}

void WebGLCompressedTextureS3tc::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
