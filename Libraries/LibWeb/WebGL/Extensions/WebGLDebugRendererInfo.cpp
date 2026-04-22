/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLDebugRendererInfo.h>
#include <LibWeb/WebGL/Extensions/WebGLDebugRendererInfo.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLDebugRendererInfo);

JS::ThrowCompletionOr<GC::Ref<JS::Object>> WebGLDebugRendererInfo::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
{
    return realm.create<WebGLDebugRendererInfo>(realm, context);
}

WebGLDebugRendererInfo::WebGLDebugRendererInfo(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context)
    : PlatformObject(realm)
    , m_context(context)
{
}

void WebGLDebugRendererInfo::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLDebugRendererInfo);
    Base::initialize(realm);
}

void WebGLDebugRendererInfo::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
