/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/WebGL/Extensions/OESStandardDerivatives.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(OESStandardDerivatives);

JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> OESStandardDerivatives::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    auto extension = realm.create<OESStandardDerivatives>(realm, context);
    return GC::Ref<Bindings::Wrappable> { extension };
}

OESStandardDerivatives::OESStandardDerivatives(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : Wrappable(realm)
    , m_context(context)
{
}

void OESStandardDerivatives::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
