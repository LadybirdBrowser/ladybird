/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextMetricsPrototype.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextMetrics);

GC::Ref<TextMetrics> TextMetrics::create(JS::Realm& realm)
{
    return realm.create<TextMetrics>(realm);
}

TextMetrics::TextMetrics(JS::Realm& realm)
    : PlatformObject(realm)
{
}

TextMetrics::~TextMetrics() = default;

void TextMetrics::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextMetrics);
}

}
