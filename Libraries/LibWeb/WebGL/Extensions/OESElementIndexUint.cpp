/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OESElementIndexUint.h>
#include <LibWeb/WebGL/Extensions/OESElementIndexUint.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(OESElementIndexUint);

GC::Ref<Bindings::Wrappable> OESElementIndexUint::create(GC::Ref<WebGLRenderingContextBase> context)
{
    auto& realm = context->realm();
    return realm.create<OESElementIndexUint>(realm, context);
}

OESElementIndexUint::OESElementIndexUint(JS::Realm&, GC::Ref<WebGLRenderingContextBase> context)
    : m_context(context)
{
}

void OESElementIndexUint::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
