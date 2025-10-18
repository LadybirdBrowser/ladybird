/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTRenderSnormPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTRenderSnorm.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(EXTRenderSnorm);

JS::ThrowCompletionOr<GC::Ptr<EXTRenderSnorm>> EXTRenderSnorm::create(JS::Realm& realm, GC::Ref<WebGL2RenderingContext> context)
{
    return realm.create<EXTRenderSnorm>(realm, context);
}

EXTRenderSnorm::EXTRenderSnorm(JS::Realm& realm, GC::Ref<WebGL2RenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_render_snorm");
}

void EXTRenderSnorm::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(EXTRenderSnorm);
    Base::initialize(realm);
}

void EXTRenderSnorm::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
