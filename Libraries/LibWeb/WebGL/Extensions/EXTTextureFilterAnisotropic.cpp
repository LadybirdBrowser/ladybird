/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTTextureFilterAnisotropicPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTTextureFilterAnisotropic.h>
#include <LibWeb/WebGL/OpenGLContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(EXTTextureFilterAnisotropic);

JS::ThrowCompletionOr<GC::Ptr<EXTTextureFilterAnisotropic>> EXTTextureFilterAnisotropic::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    return realm.create<EXTTextureFilterAnisotropic>(realm, context);
}

EXTTextureFilterAnisotropic::EXTTextureFilterAnisotropic(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_texture_filter_anisotropic");
}

void EXTTextureFilterAnisotropic::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(EXTTextureFilterAnisotropic);
    Base::initialize(realm);
}

void EXTTextureFilterAnisotropic::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
