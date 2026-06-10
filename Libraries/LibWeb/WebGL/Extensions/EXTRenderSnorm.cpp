/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/Extensions/EXTRenderSnorm.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(EXTRenderSnorm);

GC::Ref<WebGLExtension> EXTRenderSnorm::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = GC::Heap::the().allocate<EXTRenderSnorm>(context);
    return GC::Ref<WebGLExtension> { extension };
}

EXTRenderSnorm::EXTRenderSnorm(GC::Ref<WebGLRenderingContextBase> context)
    : WebGLExtension()
    , m_context(context)
{
}

void EXTRenderSnorm::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
