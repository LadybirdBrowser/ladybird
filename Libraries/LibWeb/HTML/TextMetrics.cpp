/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextMetrics);

GC::Ref<TextMetrics> TextMetrics::create()
{
    return GC::Heap::the().allocate<TextMetrics>();
}

TextMetrics::TextMetrics()
{
}

TextMetrics::~TextMetrics() = default;

}
