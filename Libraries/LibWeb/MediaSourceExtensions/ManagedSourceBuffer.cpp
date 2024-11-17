/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ManagedSourceBufferPrototype.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/ManagedSourceBuffer.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(ManagedSourceBuffer);

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

// https://w3c.github.io/media-source/#dom-managedsourcebuffer-onbufferedchange
void ManagedSourceBuffer::set_onbufferedchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::bufferedchange, event_handler);
}

// https://w3c.github.io/media-source/#dom-managedsourcebuffer-onbufferedchange
GC::Ptr<WebIDL::CallbackType> ManagedSourceBuffer::onbufferedchange()
{
    return event_handler_attribute(EventNames::bufferedchange);
}

}
