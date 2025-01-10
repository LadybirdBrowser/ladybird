/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL::Extensions {

class WebGLVertexArrayObjectOES : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLVertexArrayObjectOES, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLVertexArrayObjectOES);

public:
    static GC::Ref<WebGLVertexArrayObjectOES> create(JS::Realm& realm, WebGLRenderingContextBase&, GLuint handle);

    virtual ~WebGLVertexArrayObjectOES() override;

protected:
    explicit WebGLVertexArrayObjectOES(JS::Realm&, WebGLRenderingContextBase&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
