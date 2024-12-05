/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLProgramPrototype.h>
#include <LibWeb/WebGL/WebGLProgram.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLProgram);

GC::Ref<WebGLProgram> WebGLProgram::create(JS::Realm& realm, GLuint handle)
{
    return realm.create<WebGLProgram>(realm, handle);
}

WebGLProgram::WebGLProgram(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLProgram::~WebGLProgram() = default;

void WebGLProgram::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLProgram);
}

}
