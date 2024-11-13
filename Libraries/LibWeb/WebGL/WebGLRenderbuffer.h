/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLRenderbuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLRenderbuffer, WebGLObject);
    JS_DECLARE_ALLOCATOR(WebGLRenderbuffer);

public:
    virtual ~WebGLRenderbuffer();

protected:
    explicit WebGLRenderbuffer(JS::Realm&);
};

}
