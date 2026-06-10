/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLQuery.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLQuery);

GC::Ref<WebGLQuery> WebGLQuery::create(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return GC::Heap::the().allocate<WebGLQuery>(context, handle);
}

WebGLQuery::WebGLQuery(GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(context, handle)
{
}

WebGLQuery::~WebGLQuery() = default;

}
