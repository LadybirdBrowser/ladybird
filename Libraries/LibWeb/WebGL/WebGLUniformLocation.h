/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLUniformLocation final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLUniformLocation, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLUniformLocation);

public:
    virtual ~WebGLUniformLocation();

protected:
    explicit WebGLUniformLocation(JS::Realm&);
};

}
