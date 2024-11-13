/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLFramebuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLFramebuffer, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLFramebuffer);

public:
    virtual ~WebGLFramebuffer();

protected:
    explicit WebGLFramebuffer(JS::Realm&);
};

}
