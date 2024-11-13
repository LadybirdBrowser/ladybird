/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

WebGLObject::WebGLObject(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

WebGLObject::~WebGLObject() = default;

}
