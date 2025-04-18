/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCore/Socket.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports
class MessagePort final
    : public DOM::EventTarget
    , public Bindings::Transferable {
    WEB_PLATFORM_OBJECT(MessagePort, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MessagePort);

public:
    [[nodiscard]] static GC::Ref<MessagePort> create(JS::Realm&, HTML::TransferType primary_interface = HTML::TransferType::MessagePort);

    static void for_each_message_port(Function<void(MessagePort&)>);

    virtual ~MessagePort() override;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
    void entangle_with(MessagePort&);

    void disentangle();

    GC::Ptr<MessagePort> entangled_port() { return m_remote_port; }
    GC::Ptr<MessagePort const> entangled_port() const { return m_remote_port; }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
    WebIDL::ExceptionOr<void> post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
    WebIDL::ExceptionOr<void> post_message(JS::Value message, StructuredSerializeOptions const& options);

    void start();

    void close();

    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();

    // ^Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(HTML::TransferDataHolder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(HTML::TransferDataHolder&) override;
    virtual HTML::TransferType primary_interface() const override { return m_primary_interface; }

    void set_worker_event_target(GC::Ref<DOM::EventTarget>);

    WebIDL::ExceptionOr<void> message_port_post_message_steps(GC::Ptr<MessagePort> target_port, JS::Value message, StructuredSerializeOptions const& options);

private:
    explicit MessagePort(JS::Realm&, HTML::TransferType primary_interface);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    bool is_entangled() const;

    void post_message_task_steps(SerializedTransferRecord&);
    void post_port_message(SerializedTransferRecord);
    ErrorOr<void> send_message_on_transport(SerializedTransferRecord const&);
    void read_from_transport();

    HTML::TransferType m_primary_interface { HTML::TransferType::MessagePort };

    // The HTML spec implies(!) that this is MessagePort.[[RemotePort]]
    GC::Ptr<MessagePort> m_remote_port;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#has-been-shipped
    bool m_has_been_shipped { false };

    OwnPtr<IPC::Transport> m_transport;

    GC::Ptr<DOM::EventTarget> m_worker_event_target;
};

}
