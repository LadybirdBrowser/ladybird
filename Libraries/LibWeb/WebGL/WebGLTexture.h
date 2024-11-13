/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLTexture final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLTexture, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLTexture);

public:
    virtual ~WebGLTexture();

protected:
    explicit WebGLTexture(JS::Realm&);
};

}
