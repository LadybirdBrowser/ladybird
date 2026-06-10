/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>

namespace Web::WebGL {

class WebGLDebugRendererInfo : public WebGLExtension {
    WEB_WRAPPABLE(WebGLDebugRendererInfo, WebGLExtension);
    GC_DECLARE_ALLOCATOR(WebGLDebugRendererInfo);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    WebGLDebugRendererInfo(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
