/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTTextureNorm16.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTTextureNorm16.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(EXTTextureNorm16);

GC::Ref<Bindings::Wrappable> EXTTextureNorm16::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto& realm = context->realm();
    return realm.create<EXTTextureNorm16>(realm, context);
}

EXTTextureNorm16::EXTTextureNorm16(JS::Realm&, GC::Ref<WebGLRenderingContextBase> context)
    : m_context(context)
{
}

void EXTTextureNorm16::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
