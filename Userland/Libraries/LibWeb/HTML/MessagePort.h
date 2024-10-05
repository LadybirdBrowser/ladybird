/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCore/Socket.h>
#include <LibIPC/File.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

#define ENUMERATE_MESSAGE_PORT_EVENT_HANDLERS(E) \
    E(onmessage, HTML::EventNames::message)      \
    E(onmessageerror, HTML::EventNames::messageerror)

// https://html.spec.whatwg.org/multipage/web-messaging.html#structuredserializeoptions
struct StructuredSerializeOptions {
    Vector<JS::Handle<JS::Object>> transfer;
};

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports
class MessagePort final : public DOM::EventTarget
    , public Bindings::Transferable {
    WEB_PLATFORM_OBJECT(MessagePort, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(MessagePort);

public:
    [[nodiscard]] static JS::NonnullGCPtr<MessagePort> create(JS::Realm&);

    virtual ~MessagePort() override;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
    void entangle_with(MessagePort&);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
    WebIDL::ExceptionOr<void> post_message(JS::Value message, Vector<JS::Handle<JS::Object>> const& transfer);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
    WebIDL::ExceptionOr<void> post_message(JS::Value message, StructuredSerializeOptions const& options);

    void start();

    void close();

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_MESSAGE_PORT_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

    // ^Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(HTML::TransferDataHolder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(HTML::TransferDataHolder&) override;
    virtual HTML::TransferType primary_interface() const override { return HTML::TransferType::MessagePort; }

    void set_worker_event_target(JS::NonnullGCPtr<DOM::EventTarget>);

private:
    explicit MessagePort(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    bool is_entangled() const { return static_cast<bool>(m_socket); }
    void disentangle();

    WebIDL::ExceptionOr<void> message_port_post_message_steps(JS::GCPtr<MessagePort> target_port, JS::Value message, StructuredSerializeOptions const& options);
    void post_message_task_steps(SerializedTransferRecord&);
    void post_port_message(SerializedTransferRecord);
    ErrorOr<void> send_message_on_socket(SerializedTransferRecord const&);
    void read_from_socket();

    enum class ParseDecision {
        NotEnoughData,
        ParseNextMessage,
    };
    ErrorOr<ParseDecision> parse_message();

    // The HTML spec implies(!) that this is MessagePort.[[RemotePort]]
    JS::GCPtr<MessagePort> m_remote_port;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#has-been-shipped
    bool m_has_been_shipped { false };

    OwnPtr<Core::LocalSocket> m_socket;

    enum class SocketState : u8 {
        Header,
        Data,
        Error,
    } m_socket_state { SocketState::Header };
    size_t m_socket_incoming_message_size { 0 };
    Queue<IPC::File> m_unprocessed_fds;
    Vector<u8> m_buffered_data;

    JS::GCPtr<DOM::EventTarget> m_worker_event_target;
};

}
