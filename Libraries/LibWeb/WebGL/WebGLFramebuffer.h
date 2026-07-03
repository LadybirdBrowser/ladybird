/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLFramebuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLFramebuffer, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLFramebuffer);

public:
    struct Attachment {
        GC::Ptr<WebGLTexture> texture;
        GC::Ptr<WebGLRenderbuffer> renderbuffer;
        GLenum texture_target { 0 };
        GLint texture_level { 0 };
    };

    static GC::Ref<WebGLFramebuffer> create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLFramebuffer();

    Attachment const* attachment(GLenum attachment) const;
    void set_renderbuffer_attachment(GLenum attachment, GC::Ptr<WebGLRenderbuffer>);
    void set_texture_attachment(GLenum attachment, GC::Ptr<WebGLTexture>, GLenum texture_target, GLint level);

protected:
    explicit WebGLFramebuffer(JS::Realm&, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    HashMap<GLenum, Attachment> m_attachments;
};

}
