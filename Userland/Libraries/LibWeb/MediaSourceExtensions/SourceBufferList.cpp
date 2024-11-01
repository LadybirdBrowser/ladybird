/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferListPrototype.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>

namespace Web::MediaSourceExtensions {

JS_DEFINE_ALLOCATOR(SourceBufferList);

SourceBufferList::SourceBufferList(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

SourceBufferList::~SourceBufferList() = default;

void SourceBufferList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBufferList);
}

}
