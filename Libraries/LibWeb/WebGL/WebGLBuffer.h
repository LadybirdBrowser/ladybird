/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLBuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLBuffer, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLBuffer);

public:
    static GC::Ref<WebGLBuffer> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLBuffer();

protected:
    explicit WebGLBuffer(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
