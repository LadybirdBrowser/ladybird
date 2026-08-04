/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTRenderSnorm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTRenderSnorm.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(EXTRenderSnorm);

GC::Ref<Bindings::Wrappable> EXTRenderSnorm::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto& realm = context->realm();
    return realm.create<EXTRenderSnorm>(realm, context);
}

EXTRenderSnorm::EXTRenderSnorm(JS::Realm&, GC::Ref<WebGLRenderingContextBase> context)
    : m_context(context)
{
}

void EXTRenderSnorm::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
