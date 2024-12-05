/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLUniformLocationPrototype.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLUniformLocation);

GC::Ref<WebGLUniformLocation> WebGLUniformLocation::create(JS::Realm& realm, GLuint handle)
{
    return realm.create<WebGLUniformLocation>(realm, handle);
}

WebGLUniformLocation::WebGLUniformLocation(JS::Realm& realm, GLuint handle)
    : Bindings::PlatformObject(realm)
    , m_handle(handle)
{
}

WebGLUniformLocation::~WebGLUniformLocation() = default;

void WebGLUniformLocation::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLUniformLocation);
}

}
