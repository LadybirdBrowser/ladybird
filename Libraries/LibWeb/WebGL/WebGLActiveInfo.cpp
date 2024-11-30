/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLActiveInfoPrototype.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLActiveInfo);

GC::Ptr<WebGLActiveInfo> WebGLActiveInfo::create(JS::Realm& realm, String name, GLenum type, GLsizei size)
{
    return realm.create<WebGLActiveInfo>(realm, move(name), type, size);
}

WebGLActiveInfo::WebGLActiveInfo(JS::Realm& realm, String name, GLenum type, GLsizei size)
    : Bindings::PlatformObject(realm)
    , m_name(move(name))
    , m_type(type)
    , m_size(size)
{
}

WebGLActiveInfo::~WebGLActiveInfo() = default;

void WebGLActiveInfo::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLActiveInfo);
}

}
