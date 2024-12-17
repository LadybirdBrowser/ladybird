/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLSyncPrototype.h>
#include <LibWeb/WebGL/WebGLSync.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLSync);

GC::Ref<WebGLSync> WebGLSync::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLsyncInternal handle)
{
    return realm.create<WebGLSync>(realm, context, handle);
}

WebGLSync::WebGLSync(JS::Realm& realm, WebGLRenderingContextBase& context, GLsyncInternal handle)
    : WebGLObject(realm, context, 0)
    , m_sync_handle(handle)
{
}

WebGLSync::~WebGLSync() = default;

void WebGLSync::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLSync);
}

}
