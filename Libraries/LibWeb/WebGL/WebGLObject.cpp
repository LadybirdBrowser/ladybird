/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLObjectPrototype.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

WebGLObject::WebGLObject(JS::Realm& realm, GLuint handle)
    : Bindings::PlatformObject(realm)
    , m_handle(handle)
{
}

WebGLObject::~WebGLObject() = default;

void WebGLObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLObject);
}

}
