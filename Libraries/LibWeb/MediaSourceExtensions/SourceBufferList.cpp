/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBufferList);

SourceBufferList::SourceBufferList(MediaSource& media_source)
    : DOM::EventTarget()
    , m_media_source(media_source)
{
}

SourceBufferList::~SourceBufferList() = default;

GC::Ptr<Bindings::Wrappable> SourceBufferList::relevant_global_impl() const
{
    return static_cast<Bindings::Wrappable&>(*m_media_source).relevant_global_impl();
}

void SourceBufferList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_source);
    visitor.visit(m_buffers);
}

void SourceBufferList::append(GC::Ref<SourceBuffer> buffer)
{
    m_buffers.append(buffer);
}

void SourceBufferList::insert_in_order_of(GC::Ref<SourceBuffer> buffer, SourceBufferList const& ordered_list)
{
    VERIFY(!contains(buffer));
    VERIFY(ordered_list.contains(buffer));

    size_t insertion_index = 0;
    for (auto ordered_buffer : ordered_list.m_buffers) {
        if (ordered_buffer == buffer)
            break;
        if (contains(ordered_buffer))
            insertion_index++;
    }
    m_buffers.insert(insertion_index, buffer);
}

void SourceBufferList::remove(SourceBuffer const& buffer)
{
    auto removed = m_buffers.remove_first_matching([&](GC::Ref<SourceBuffer> contained_buffer) {
        return contained_buffer.ptr() == &buffer;
    });
    VERIFY(removed);
}

void SourceBufferList::remove_all_buffers(Badge<MediaSource>)
{
    m_buffers.clear();
}

size_t SourceBufferList::length() const
{
    return m_buffers.size();
}

// https://w3c.github.io/media-source/#dfn-sourcebufferlist-getter
GC::Ptr<SourceBuffer> SourceBufferList::item(u32 index) const
{
    // 1. If index is greater than or equal to the length attribute then return undefined and abort these steps.
    if (index >= m_buffers.size())
        return nullptr;

    // 2. Return the index'th SourceBuffer object in the list.
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
    return m_buffers.contains([&](GC::Ref<SourceBuffer> contained_buffer) {
        return contained_buffer == GC::Ref { source_buffer };
    });
}

}
