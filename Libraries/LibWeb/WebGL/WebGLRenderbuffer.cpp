/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLRenderbufferPrototype.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLRenderbuffer);

WebGLRenderbuffer::WebGLRenderbuffer(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLRenderbuffer::~WebGLRenderbuffer() = default;

}
