/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextMetrics);

GC::Ref<TextMetrics> TextMetrics::create(JS::Realm& realm)
{
    return realm.create<TextMetrics>(realm);
}

TextMetrics::TextMetrics(JS::Realm& realm)
    : Wrappable(realm)
{
}

TextMetrics::~TextMetrics() = default;

}
