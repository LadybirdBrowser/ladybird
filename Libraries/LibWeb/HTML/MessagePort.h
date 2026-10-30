/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports
class WEB_API MessagePort : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(MessagePort, message_port)

public:
    [[nodiscard]] static GC::Ref<MessagePort> create(JS::Realm&);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:transfer-receiving-steps
    [[nodiscard]] static GC::Ref<MessagePort> create_from_transfer(JS::Realm&, TransferDataDecoder&);

    static void disentangle_ports_of_realm(JS::Realm&);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
    void entangle_with(MessagePort&);

    GC::Ptr<MessagePort> entangled_port();

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-event-target
    void set_message_event_target(DOM::EventTarget&);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer);

    void enable();

    void close();

    void discard_pending_messages();

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:transfer-steps
    WebIDL::ExceptionOr<void> transfer_steps(JS::Realm&, TransferDataEncoder&);

    // https://html.spec.whatwg.org/multipage/structured-data.html#detached
    bool is_detached() const;

    size_t pending_outgoing_message_count() const;
    void fail_next_transfer_for_testing();
};

}

namespace Web::Bindings {

WEB_API GC::RootVector<GC::Ref<HTML::MessagePort>> message_ports_from_transferred_values(Vector<GC::Root<JS::Object>> const&);

}
