/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WebGLSync.h>
#include <LibWeb/WebGL/WebGLSync.h>

#include <GLES2/gl2.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLSync);

GC::Ref<WebGLSync> WebGLSync::create(GC::Ref<WebGLRenderingContextBase> context, GLsyncInternal handle)
{
    return GC::Heap::the().allocate<WebGLSync>(context, handle);
}

WebGLSync::WebGLSync(GC::Ref<WebGLRenderingContextBase> context, GLsyncInternal handle)
    : WebGLObject(context, 0)
    , m_sync_handle(handle)
{
}

WebGLSync::~WebGLSync() = default;

ErrorOr<GLsyncInternal> WebGLSync::sync_handle(WebGLRenderingContextBase const* context) const
{
    if (context == m_context)
        return m_sync_handle;
    return Error::from_errno(GL_INVALID_OPERATION);
}

}
