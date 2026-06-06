/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WebGLDebugRendererInfo.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class WebGLDebugRendererInfo : public Bindings::Wrappable {
    WEB_WRAPPABLE(WebGLDebugRendererInfo, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(WebGLDebugRendererInfo);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    WebGLDebugRendererInfo(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
