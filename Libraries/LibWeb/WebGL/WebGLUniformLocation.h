/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WebGLUniformLocation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLUniformLocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLUniformLocation);

public:
    static GC::Ref<WebGLUniformLocation> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLUniformLocation();

    GLuint handle() const { return m_handle; }

protected:
    explicit WebGLUniformLocation(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;

    GLuint m_handle { 0 };
};

}
