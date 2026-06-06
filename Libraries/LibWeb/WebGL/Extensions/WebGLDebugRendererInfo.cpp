/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/Extensions/WebGLDebugRendererInfo.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLDebugRendererInfo);

JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> WebGLDebugRendererInfo::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = GC::Heap::the().allocate<WebGLDebugRendererInfo>(context);
    return GC::Ref<Bindings::Wrappable> { extension };
}

WebGLDebugRendererInfo::WebGLDebugRendererInfo(GC::Ref<WebGLRenderingContextBase> context)
    : Bindings::Wrappable()
    , m_context(context)
{
}

void WebGLDebugRendererInfo::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
