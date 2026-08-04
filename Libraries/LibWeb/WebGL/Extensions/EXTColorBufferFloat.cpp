/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTColorBufferFloat.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTColorBufferFloat.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(EXTColorBufferFloat);

GC::Ref<Bindings::Wrappable> EXTColorBufferFloat::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto& realm = context->realm();
    return realm.create<EXTColorBufferFloat>(realm, context);
}

EXTColorBufferFloat::EXTColorBufferFloat(JS::Realm&, GC::Ref<WebGLRenderingContextBase> context)
    : m_context(context)
{
}

void EXTColorBufferFloat::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
