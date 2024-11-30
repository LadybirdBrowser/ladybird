/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLProgramPrototype.h>
#include <LibWeb/WebGL/WebGLProgram.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLProgram);

GC::Ptr<WebGLProgram> WebGLProgram::create(JS::Realm& realm, GLuint handle)
{
    return realm.heap().allocate<WebGLProgram>(realm, handle);
}

WebGLProgram::WebGLProgram(JS::Realm& realm, GLuint handle)
    : WebGLObject(realm, handle)
{
}

WebGLProgram::~WebGLProgram() = default;

}
