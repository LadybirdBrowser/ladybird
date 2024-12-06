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
    WEB_PLATFORM_OBJECT(WebGLSampler, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLSampler);

public:
    static GC::Ref<WebGLSampler> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLSampler() override;

protected:
    explicit WebGLSampler(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
