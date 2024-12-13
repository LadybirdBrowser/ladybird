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
    static GC::Ref<WebGLSync> create(JS::Realm& realm, GLsyncInternal handle);

    virtual ~WebGLSync() override;

    GLsyncInternal sync_handle() const { return m_sync_handle; }

protected:
    explicit WebGLSync(JS::Realm&, GLsyncInternal handle);

    virtual void initialize(JS::Realm&) override;

    GLsyncInternal m_sync_handle { nullptr };
};

}
