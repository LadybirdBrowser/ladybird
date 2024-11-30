/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLTexturePrototype.h>
#include <LibWeb/WebGL/WebGLTexture.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLTexture);

GC::Ptr<WebGLTexture> WebGLTexture::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLTexture>(realm, handle);
}

WebGLTexture::WebGLTexture(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLTexture::~WebGLTexture() = default;

}
