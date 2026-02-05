/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OESStandardDerivativesPrototype.h>
#include <LibWeb/WebGL/Extensions/OESStandardDerivatives.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(OESStandardDerivatives);

JS::ThrowCompletionOr<GC::Ptr<OESStandardDerivatives>> OESStandardDerivatives::create(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
{
    return realm.create<OESStandardDerivatives>(realm, context);
}

OESStandardDerivatives::OESStandardDerivatives(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_OES_standard_derivatives");
}

void OESStandardDerivatives::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OESStandardDerivatives);
    Base::initialize(realm);
}

void OESStandardDerivatives::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
