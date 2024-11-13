/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLShaderPrecisionFormat final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLShaderPrecisionFormat, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLShaderPrecisionFormat);

public:
    virtual ~WebGLShaderPrecisionFormat();

protected:
    explicit WebGLShaderPrecisionFormat(JS::Realm&);
};

}
