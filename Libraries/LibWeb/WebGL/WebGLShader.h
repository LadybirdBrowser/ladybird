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

class WebGLShader final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLShader, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLShader);

public:
    static GC::Ref<WebGLShader> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLShader();

protected:
    explicit WebGLShader(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
