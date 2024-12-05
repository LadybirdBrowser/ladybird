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

class WebGLTexture final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLTexture, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLTexture);

public:
    static GC::Ref<WebGLTexture> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLTexture();

protected:
    explicit WebGLTexture(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
