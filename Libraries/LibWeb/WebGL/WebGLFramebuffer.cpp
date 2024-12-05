/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLFramebufferPrototype.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLFramebuffer);

GC::Ptr<WebGLFramebuffer> WebGLFramebuffer::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLFramebuffer>(realm, handle);
}

WebGLFramebuffer::WebGLFramebuffer(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLFramebuffer::~WebGLFramebuffer() = default;

}
