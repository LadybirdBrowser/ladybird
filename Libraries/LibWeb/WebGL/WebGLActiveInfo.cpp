/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebGL/WebGLActiveInfo.h>

namespace Web::WebGL {

JS_DEFINE_ALLOCATOR(WebGLActiveInfo);

WebGLActiveInfo::WebGLActiveInfo(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

WebGLActiveInfo::~WebGLActiveInfo() = default;

}
