/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLTexture.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLFramebuffer);

GC::Ref<WebGLFramebuffer> WebGLFramebuffer::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return realm.create<WebGLFramebuffer>(realm, context, handle);
}

WebGLFramebuffer::WebGLFramebuffer(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLFramebuffer::~WebGLFramebuffer() = default;

void WebGLFramebuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLFramebuffer);
    Base::initialize(realm);
}

void WebGLFramebuffer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& attachment : m_attachments) {
        visitor.visit(attachment.value.texture);
        visitor.visit(attachment.value.renderbuffer);
    }
}

WebGLFramebuffer::Attachment const* WebGLFramebuffer::attachment(GLenum attachment) const
{
    auto it = m_attachments.find(attachment);
    if (it == m_attachments.end())
        return nullptr;
    return &it->value;
}

void WebGLFramebuffer::set_renderbuffer_attachment(GLenum attachment, GC::Ptr<WebGLRenderbuffer> renderbuffer)
{
    if (!renderbuffer) {
        m_attachments.remove(attachment);
        return;
    }

    Attachment framebuffer_attachment;
    framebuffer_attachment.renderbuffer = renderbuffer;
    m_attachments.set(attachment, framebuffer_attachment);
}

void WebGLFramebuffer::set_texture_attachment(GLenum attachment, GC::Ptr<WebGLTexture> texture, GLenum texture_target, GLint level)
{
    if (!texture) {
        m_attachments.remove(attachment);
        return;
    }

    Attachment framebuffer_attachment;
    framebuffer_attachment.texture = texture;
    framebuffer_attachment.texture_target = texture_target;
    framebuffer_attachment.texture_level = level;
    m_attachments.set(attachment, framebuffer_attachment);
}

}
