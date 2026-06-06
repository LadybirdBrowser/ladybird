/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLRenderbuffer final : public WebGLObject {
    WEB_WRAPPABLE(WebGLRenderbuffer, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLRenderbuffer);

public:
    static GC::Ref<WebGLRenderbuffer> create(GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLRenderbuffer();

protected:
    explicit WebGLRenderbuffer(GC::Ref<WebGLRenderingContextBase>, GLuint handle);
};

}
