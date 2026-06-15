/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLSync.h>
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
    TRY(validate_context(context));
    if (invalidated_for_context(context))
        return Error::from_errno(GL_INVALID_OPERATION);
    return m_sync_handle;
}

ErrorOr<Optional<GLsyncInternal>> WebGLSync::sync_handle_for_deletion(WebGLRenderingContextBase const* context)
{
    TRY(validate_context(context));
    if (invalidated_for_context(context))
        return Optional<GLsyncInternal> {};
    invalidate();
    return Optional<GLsyncInternal> { m_sync_handle };
}

}
