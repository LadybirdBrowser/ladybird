/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WebGLTexturePrototype.h>
#include <LibWeb/WebGL/WebGLTexture.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLTexture);

WebGLTexture::WebGLTexture(JS::Realm& realm)
    : WebGLObject(realm)
{
}

WebGLTexture::~WebGLTexture() = default;

}
