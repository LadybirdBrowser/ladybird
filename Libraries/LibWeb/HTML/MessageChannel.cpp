/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/MessageChannel.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MessageChannel);

GC::Ref<MessageChannel> MessageChannel::create(GC::Ref<DOM::EventTarget> relevant_global_object)
{
    auto port1 = MessagePort::create(relevant_global_object);
    auto port2 = MessagePort::create(relevant_global_object);
    auto channel = GC::Heap::the().allocate<MessageChannel>(port1, port2);

    // 3. Entangle this's port 1 and this's port 2.
    channel->m_port1->entangle_with(*channel->m_port2);

    return channel;
}

GC::Ref<MessageChannel> MessageChannel::create_for_constructor(JS::Realm& realm)
{
    auto* global_scope = window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    return create(global_scope->this_impl());
}

MessageChannel::MessageChannel(GC::Ref<MessagePort> port1, GC::Ref<MessagePort> port2)
    : m_port1(port1)
    , m_port2(port2)
{
}

MessageChannel::~MessageChannel() = default;

void MessageChannel::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port1);
    visitor.visit(m_port2);
}

MessagePort* MessageChannel::port1()
{
    return m_port1;
}

MessagePort* MessageChannel::port2()
{
    return m_port2;
}

MessagePort const* MessageChannel::port1() const
{
    return m_port1;
}

MessagePort const* MessageChannel::port2() const
{
    return m_port2;
}

}
