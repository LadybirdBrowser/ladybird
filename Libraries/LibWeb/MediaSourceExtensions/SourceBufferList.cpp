/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferList.h>
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
    visitor.visit(m_buffers);
}

void SourceBufferList::append(GC::Ref<SourceBuffer> buffer)
{
    // From https://w3c.github.io/media-source/#addsourcebuffer-method
    // 8. Append buffer to this's sourceBuffers.
    m_buffers.append(buffer);

    // 9. Queue a task to fire an event named addsourcebuffer at this's sourceBuffers.
    // FIXME: Should this have a task source? An event loop? A document?
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(heap(), [weak_self = GC::Weak(*this)] {
        if (!weak_self)
            return;
        weak_self->dispatch_event(DOM::Event::create(weak_self->realm(), EventNames::addsourcebuffer));
    }));
}

size_t SourceBufferList::length() const
{
    return m_buffers.size();
}

GC::Ref<SourceBuffer> const& SourceBufferList::item(u32 index) const
{
    return m_buffers[index];
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

bool SourceBufferList::contains(SourceBuffer const& source_buffer) const
{
    return m_buffers.contains([&](GC::Ref<SourceBuffer> const& contained_buffer) { return contained_buffer == &source_buffer; });
}

}
