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
    WEB_WRAPPABLE(WebGLSync, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLSync);

public:
    static GC::Ref<WebGLSync> create(GC::Ref<WebGLRenderingContextBase>, GLsyncInternal handle);

    virtual ~WebGLSync() override;

    ErrorOr<GLsyncInternal> sync_handle(WebGLRenderingContextBase const* context) const;

protected:
    explicit WebGLSync(GC::Ref<WebGLRenderingContextBase>, GLsyncInternal handle);

    GLsyncInternal m_sync_handle { nullptr };
};

}
