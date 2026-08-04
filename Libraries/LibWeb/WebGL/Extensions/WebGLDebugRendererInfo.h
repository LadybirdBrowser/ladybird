/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class WebGLDebugRendererInfo : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(WebGLDebugRendererInfo, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(WebGLDebugRendererInfo);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(Visitor&) override;

private:
    WebGLDebugRendererInfo(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
