/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-channels
class MessageChannel final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MessageChannel, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MessageChannel);

public:
    static WebIDL::ExceptionOr<GC::Ref<MessageChannel>> construct_impl(JS::Realm&);
    virtual ~MessageChannel() override;

    MessagePort* port1();
    MessagePort const* port1() const;

    MessagePort* port2();
    MessagePort const* port2() const;

private:
    explicit MessageChannel(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<MessagePort> m_port1;
    GC::Ptr<MessagePort> m_port2;
};

}
