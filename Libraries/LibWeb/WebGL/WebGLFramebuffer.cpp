/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLFramebufferPrototype.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLFramebuffer);

GC::Ref<WebGLFramebuffer> WebGLFramebuffer::create(JS::Realm& realm, GLuint handle)
{
    return realm.create<WebGLFramebuffer>(realm, handle);
}

WebGLFramebuffer::WebGLFramebuffer(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLFramebuffer::~WebGLFramebuffer() = default;

void WebGLFramebuffer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLFramebuffer);
}

}
