/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLBuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLBuffer, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLBuffer);

public:
    virtual ~WebGLBuffer();

protected:
    explicit WebGLBuffer(JS::Realm&);
};

}
