/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTTextureNorm16Prototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTTextureNorm16.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(EXTTextureNorm16);

JS::ThrowCompletionOr<GC::Ptr<EXTTextureNorm16>> EXTTextureNorm16::create(JS::Realm& realm, GC::Ref<WebGL2RenderingContext> context)
{
    return realm.create<EXTTextureNorm16>(realm, context);
}

EXTTextureNorm16::EXTTextureNorm16(JS::Realm& realm, GC::Ref<WebGL2RenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_texture_norm16");
}

void EXTTextureNorm16::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(EXTTextureNorm16);
    Base::initialize(realm);
}

void EXTTextureNorm16::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
