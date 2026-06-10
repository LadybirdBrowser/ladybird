/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WebGLDrawBuffers : public WebGLExtension {
    WEB_WRAPPABLE(WebGLDrawBuffers, WebGLExtension);
    GC_DECLARE_ALLOCATOR(WebGLDrawBuffers);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

    void draw_buffers_webgl(Vector<GLenum> buffers);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    WebGLDrawBuffers(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
