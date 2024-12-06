/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLTransformFeedback : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLTransformFeedback, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLTransformFeedback);

public:
    static GC::Ref<WebGLTransformFeedback> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLTransformFeedback() override;

protected:
    explicit WebGLTransformFeedback(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
