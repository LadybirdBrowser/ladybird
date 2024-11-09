/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessageChannelPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/MessageChannel.h>
#include <LibWeb/HTML/MessagePort.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(MessageChannel);

WebIDL::ExceptionOr<JS::NonnullGCPtr<MessageChannel>> MessageChannel::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<MessageChannel>(realm, realm);
}

MessageChannel::MessageChannel(JS::Realm& realm)
    : PlatformObject(realm)
{
    // 1. Set this's port 1 to a new MessagePort in this's relevant Realm.
    m_port1 = MessagePort::create(realm);

    // 2. Set this's port 2 to a new MessagePort in this's relevant Realm.
    m_port2 = MessagePort::create(realm);

    // 3. Entangle this's port 1 and this's port 2.
    m_port1->entangle_with(*m_port2);
}

MessageChannel::~MessageChannel() = default;

void MessageChannel::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port1);
    visitor.visit(m_port2);
}

void MessageChannel::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MessageChannel);
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
