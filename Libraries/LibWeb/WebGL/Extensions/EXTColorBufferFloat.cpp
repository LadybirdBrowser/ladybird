/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/EXTColorBufferFloatPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/EXTColorBufferFloat.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(EXTColorBufferFloat);

JS::ThrowCompletionOr<GC::Ptr<EXTColorBufferFloat>> EXTColorBufferFloat::create(JS::Realm& realm, GC::Ref<WebGL2RenderingContext> context)
{
    return realm.create<EXTColorBufferFloat>(realm, context);
}

EXTColorBufferFloat::EXTColorBufferFloat(JS::Realm& realm, GC::Ref<WebGL2RenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_EXT_color_buffer_float");
}

void EXTColorBufferFloat::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(EXTColorBufferFloat);
}

void EXTColorBufferFloat::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
