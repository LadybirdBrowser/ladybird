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

GC::Ref<WebGLSync> WebGLSync::create(JS::Realm& realm, GLuint handle)
{
    return realm.create<WebGLSync>(realm, handle);
}

WebGLSync::WebGLSync(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLSync::~WebGLSync() = default;

void WebGLSync::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLSync);
}

}
