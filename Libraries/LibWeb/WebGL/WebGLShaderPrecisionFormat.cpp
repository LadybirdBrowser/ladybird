/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLShaderPrecisionFormatPrototype.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLShaderPrecisionFormat);

GC::Ref<WebGLShaderPrecisionFormat> WebGLShaderPrecisionFormat::create(JS::Realm& realm, GLint range_min, GLint range_max, GLint precision)
{
    return realm.create<WebGLShaderPrecisionFormat>(realm, range_min, range_max, precision);
}

WebGLShaderPrecisionFormat::WebGLShaderPrecisionFormat(JS::Realm& realm, GLint range_min, GLint range_max, GLint precision)
    : Bindings::PlatformObject(realm)
    , m_range_min(range_min)
    , m_range_max(range_max)
    , m_precision(precision)
{
}

WebGLShaderPrecisionFormat::~WebGLShaderPrecisionFormat() = default;

void WebGLShaderPrecisionFormat::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLShaderPrecisionFormat);
}

}
