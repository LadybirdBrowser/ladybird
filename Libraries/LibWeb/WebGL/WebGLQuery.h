/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLQuery : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLQuery, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLQuery);

public:
    static GC::Ref<WebGLQuery> create(JS::Realm& realm, GLuint handle);

    virtual ~WebGLQuery() override;

protected:
    explicit WebGLQuery(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
};

}
