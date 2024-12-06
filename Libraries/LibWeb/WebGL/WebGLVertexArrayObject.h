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
    WEB_PLATFORM_OBJECT(WebGLVertexArrayObject, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLVertexArrayObject);

public:
    static GC::Ref<WebGLVertexArrayObject> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLVertexArrayObject() override;

protected:
    explicit WebGLVertexArrayObject(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
