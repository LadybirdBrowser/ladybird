/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTTextureFilterAnisotropic.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTTextureFilterAnisotropic.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(EXTTextureFilterAnisotropic);

GC::Ref<Bindings::Wrappable> EXTTextureFilterAnisotropic::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto& realm = context->realm();
    return realm.create<EXTTextureFilterAnisotropic>(realm, context);
}

EXTTextureFilterAnisotropic::EXTTextureFilterAnisotropic(JS::Realm&, GC::Ref<WebGLRenderingContextBase> context)
    : m_context(context)
{
}

void EXTTextureFilterAnisotropic::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
