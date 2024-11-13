/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLFramebufferPrototype.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLFramebuffer);

WebGLFramebuffer::WebGLFramebuffer(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLFramebuffer::~WebGLFramebuffer() = default;

}
