/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLSampler : public WebGLObject {
    WEB_WRAPPABLE(WebGLSampler, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLSampler);

public:
    static GC::Ref<WebGLSampler> create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle);

    virtual ~WebGLSampler() override;

protected:
    explicit WebGLSampler(GC::Ref<WebGLRenderingContextBase>, GLuint handle);
};

}
