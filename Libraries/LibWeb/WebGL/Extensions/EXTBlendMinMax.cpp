/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/Extensions/EXTBlendMinMax.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(EXTBlendMinMax);

JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> EXTBlendMinMax::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = GC::Heap::the().allocate<EXTBlendMinMax>(context);
    return GC::Ref<Bindings::Wrappable> { extension };
}

EXTBlendMinMax::EXTBlendMinMax(GC::Ref<WebGLRenderingContextBase> context)
    : Bindings::Wrappable()
    , m_context(context)
{
}

void EXTBlendMinMax::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
