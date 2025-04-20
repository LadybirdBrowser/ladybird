/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTBlendMinMaxPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTBlendMinMax.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(EXTBlendMinMax);

JS::ThrowCompletionOr<GC::Ptr<EXTBlendMinMax>> EXTBlendMinMax::create(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
{
    return realm.create<EXTBlendMinMax>(realm, context);
}

EXTBlendMinMax::EXTBlendMinMax(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_blend_minmax");
}

void EXTBlendMinMax::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(EXTBlendMinMax);
    Base::initialize(realm);
}

void EXTBlendMinMax::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
