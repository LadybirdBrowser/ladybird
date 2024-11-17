/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferListPrototype.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBufferList);

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

// https://w3c.github.io/media-source/#dom-sourcebufferlist-onaddsourcebuffer
void SourceBufferList::set_onaddsourcebuffer(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::addsourcebuffer, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebufferlist-onaddsourcebuffer
GC::Ptr<WebIDL::CallbackType> SourceBufferList::onaddsourcebuffer()
{
    return event_handler_attribute(EventNames::addsourcebuffer);
}

// https://w3c.github.io/media-source/#dom-sourcebufferlist-onremovesourcebuffer
void SourceBufferList::set_onremovesourcebuffer(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::removesourcebuffer, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebufferlist-onremovesourcebuffer
GC::Ptr<WebIDL::CallbackType> SourceBufferList::onremovesourcebuffer()
{
    return event_handler_attribute(EventNames::removesourcebuffer);
}

}
