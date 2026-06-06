/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLFramebuffer final : public WebGLObject {
    WEB_WRAPPABLE(WebGLFramebuffer, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLFramebuffer);

public:
    static GC::Ref<WebGLFramebuffer> create(GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLFramebuffer();

protected:
    explicit WebGLFramebuffer(GC::Ref<WebGLRenderingContextBase>, GLuint handle);
};

}
