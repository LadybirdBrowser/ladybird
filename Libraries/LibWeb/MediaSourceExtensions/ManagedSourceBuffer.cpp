/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ManagedSourceBufferPrototype.h>
#include <LibWeb/MediaSourceExtensions/ManagedSourceBuffer.h>

namespace Web::MediaSourceExtensions {

JS_DEFINE_ALLOCATOR(ManagedSourceBuffer);

ManagedSourceBuffer::ManagedSourceBuffer(JS::Realm& realm)
    : SourceBuffer(realm)
{
}

ManagedSourceBuffer::~ManagedSourceBuffer() = default;

void ManagedSourceBuffer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ManagedSourceBuffer);
}

}
