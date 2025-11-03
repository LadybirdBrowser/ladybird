/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

class WebGLUniformLocation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLUniformLocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLUniformLocation);

public:
    static GC::Ref<WebGLUniformLocation> create(JS::Realm& realm, WebGLRenderingContextBase&, GLuint handle);

    virtual ~WebGLUniformLocation();

    ErrorOr<GLint> handle(WebGLRenderingContextBase const* context) const;

protected:
    explicit WebGLUniformLocation(JS::Realm&, WebGLRenderingContextBase&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    // FIXME: It should be GC::Ptr instead of raw pointer, but we need to make WebGLRenderingContextBase inherit from PlatformObject first.
    WebGLRenderingContextBase* m_context;

    GLint m_handle { 0 };
};

}
