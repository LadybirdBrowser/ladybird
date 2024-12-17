/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLQueryPrototype.h>
#include <LibWeb/WebGL/WebGLQuery.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLQuery);

GC::Ref<WebGLQuery> WebGLQuery::create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
{
    return realm.create<WebGLQuery>(realm, context, handle);
}

WebGLQuery::WebGLQuery(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLQuery::~WebGLQuery() = default;

void WebGLQuery::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLQuery);
}

}
