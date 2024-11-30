/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLUniformLocation final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLUniformLocation, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLUniformLocation);

public:
    static GC::Ptr<WebGLUniformLocation> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLUniformLocation();

protected:
    explicit WebGLUniformLocation(JS::Realm&, GLuint handle);
};

}
