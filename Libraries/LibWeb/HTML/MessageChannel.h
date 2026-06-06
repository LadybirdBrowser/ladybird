/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MessageChannel.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-channels
class MessageChannel final : public Bindings::Wrappable {
    WEB_WRAPPABLE(MessageChannel, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MessageChannel);

public:
    static WebIDL::ExceptionOr<GC::Ref<MessageChannel>> construct_impl(GC::Ref<DOM::EventTarget> relevant_global_object);
    virtual ~MessageChannel() override;

    MessagePort* port1();
    MessagePort const* port1() const;

    MessagePort* port2();
    MessagePort const* port2() const;

private:
    MessageChannel(GC::Ref<MessagePort>, GC::Ref<MessagePort>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<MessagePort> m_port1;
    GC::Ptr<MessagePort> m_port2;
};

}
