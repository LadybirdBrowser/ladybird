/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLActiveInfo);

GC::Ptr<WebGLActiveInfo> WebGLActiveInfo::create(String name, GLenum type, GLsizei size)
{
    return GC::Heap::the().allocate<WebGLActiveInfo>(move(name), type, size);
}

WebGLActiveInfo::WebGLActiveInfo(String name, GLenum type, GLsizei size)
    : m_name(move(name))
    , m_type(type)
    , m_size(size)
{
}

WebGLActiveInfo::~WebGLActiveInfo() = default;

}
