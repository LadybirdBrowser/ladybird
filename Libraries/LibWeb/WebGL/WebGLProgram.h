/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLProgram final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLProgram, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLProgram);

public:
    virtual ~WebGLProgram();

protected:
    explicit WebGLProgram(JS::Realm&);
};

}
