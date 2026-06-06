/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLShaderPrecisionFormat);

GC::Ref<WebGLShaderPrecisionFormat> WebGLShaderPrecisionFormat::create(GLint range_min, GLint range_max, GLint precision)
{
    return GC::Heap::the().allocate<WebGLShaderPrecisionFormat>(range_min, range_max, precision);
}

WebGLShaderPrecisionFormat::WebGLShaderPrecisionFormat(GLint range_min, GLint range_max, GLint precision)
    : Bindings::Wrappable()
    , m_range_min(range_min)
    , m_range_max(range_max)
    , m_precision(precision)
{
}

WebGLShaderPrecisionFormat::~WebGLShaderPrecisionFormat() = default;

}
