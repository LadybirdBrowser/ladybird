/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
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
    static GC::Ref<WebGLBuffer> create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle);

    virtual ~WebGLBuffer();

    bool is_compatible_with(GLenum target);

protected:
    explicit WebGLBuffer(JS::Realm&, WebGLRenderingContextBase&, GLuint handle);

    virtual void initialize(JS::Realm&) override;

private:
    Optional<GLenum> m_target;
};

}
