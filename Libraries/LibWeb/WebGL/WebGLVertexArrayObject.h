/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLVertexArrayObject : public WebGLObject {
    WEB_WRAPPABLE(WebGLVertexArrayObject, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLVertexArrayObject);

public:
    static GC::Ref<WebGLVertexArrayObject> create(GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLVertexArrayObject() override;

protected:
    explicit WebGLVertexArrayObject(GC::Ref<WebGLRenderingContextBase>, GLuint handle);
};

}
