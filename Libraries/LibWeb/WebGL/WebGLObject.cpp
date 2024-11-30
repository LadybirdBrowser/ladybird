/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

WebGLObject::WebGLObject(JS::Realm& realm, GLuint handle)
    : Bindings::PlatformObject(realm)
    , m_handle(handle)
{
}

WebGLObject::~WebGLObject() = default;

}
