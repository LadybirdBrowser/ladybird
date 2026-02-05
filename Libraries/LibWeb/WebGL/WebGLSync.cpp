/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLSyncPrototype.h>
#include <LibWeb/WebGL/WebGLSync.h>

#include <GLES2/gl2.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLSync);

GC::Ref<WebGLSync> WebGLSync::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLsyncInternal handle)
{
    return realm.create<WebGLSync>(realm, context, handle);
}

WebGLSync::WebGLSync(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLsyncInternal handle)
    : WebGLObject(realm, context, 0)
    , m_sync_handle(handle)
{
}

WebGLSync::~WebGLSync() = default;

void WebGLSync::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLSync);
    Base::initialize(realm);
}

ErrorOr<GLsyncInternal> WebGLSync::sync_handle(WebGLRenderingContextBase const* context) const
{
    if (context == m_context)
        return m_sync_handle;
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
