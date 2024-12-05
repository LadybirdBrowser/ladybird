/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLRenderbufferPrototype.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLRenderbuffer);

GC::Ptr<WebGLRenderbuffer> WebGLRenderbuffer::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLRenderbuffer>(realm, handle);
}

WebGLRenderbuffer::WebGLRenderbuffer(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLRenderbuffer::~WebGLRenderbuffer() = default;

}
