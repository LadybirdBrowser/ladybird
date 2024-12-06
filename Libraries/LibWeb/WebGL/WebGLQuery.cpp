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

GC::Ref<WebGLQuery> WebGLQuery::create(JS::Realm& realm, GLuint handle)
{
    return realm.create<WebGLQuery>(realm, handle);
}

WebGLQuery::WebGLQuery(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLQuery::~WebGLQuery() = default;

void WebGLQuery::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLQuery);
}

}
