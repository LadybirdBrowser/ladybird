/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLSync : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLSync, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLSync);

public:
    static GC::Ref<WebGLSync> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLSync() override;

protected:
    explicit WebGLSync(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
