/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLCompressedTextureS3tc);

JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> WebGLCompressedTextureS3tc::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = GC::Heap::the().allocate<WebGLCompressedTextureS3tc>(context);
    return GC::Ref<Bindings::Wrappable> { extension };
}

WebGLCompressedTextureS3tc::WebGLCompressedTextureS3tc(GC::Ref<WebGLRenderingContextBase> context)
    : Bindings::Wrappable()
    , m_context(context)
{
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT);
    m_context->enable_compressed_texture_format(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
}

void WebGLCompressedTextureS3tc::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
