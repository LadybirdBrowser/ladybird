/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLShader final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLShader, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLShader);

public:
    virtual ~WebGLShader();

protected:
    explicit WebGLShader(JS::Realm&);
};

}
