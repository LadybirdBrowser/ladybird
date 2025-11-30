/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OESElementIndexUintPrototype.h>
#include <LibWeb/WebGL/Extensions/OESElementIndexUint.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(OESElementIndexUint);

JS::ThrowCompletionOr<GC::Ptr<OESElementIndexUint>> OESElementIndexUint::create(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
{
    return realm.create<OESElementIndexUint>(realm, context);
}

OESElementIndexUint::OESElementIndexUint(JS::Realm& realm, GC::Ref<WebGLRenderingContext> context)
    : PlatformObject(realm)
    , m_context(context)
{
    m_context->context().request_extension("GL_OES_element_index_uint");
}

void OESElementIndexUint::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OESElementIndexUint);
    Base::initialize(realm);
}

void OESElementIndexUint::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
