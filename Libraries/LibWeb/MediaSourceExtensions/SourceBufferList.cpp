/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferListPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBufferList);
    Base::initialize(realm);
}

void SourceBufferList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& source_buffer : m_source_buffers)
        visitor.visit(source_buffer);
}

// https://w3c.github.io/media-source/#dom-sourcebufferlist-item
SourceBuffer* SourceBufferList::item(size_t index)
{
    if (index >= m_source_buffers.size())
        return nullptr;
    return m_source_buffers[index].ptr();
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

// Internal methods

void SourceBufferList::add(SourceBuffer& buffer)
{
    m_source_buffers.append(buffer);

    // Fire addsourcebuffer event
    dispatch_event(DOM::Event::create(realm(), EventNames::addsourcebuffer));
}

void SourceBufferList::remove(SourceBuffer& buffer)
{
    m_source_buffers.remove_first_matching([&](auto& item) {
        return item.ptr() == &buffer;
    });

    // Fire removesourcebuffer event
    dispatch_event(DOM::Event::create(realm(), EventNames::removesourcebuffer));
}

bool SourceBufferList::contains(SourceBuffer const& buffer) const
{
    for (auto const& item : m_source_buffers) {
        if (item.ptr() == &buffer)
            return true;
    }
    return false;
}

void SourceBufferList::clear()
{
    m_source_buffers.clear();
}

}
