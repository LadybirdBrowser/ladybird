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
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

class PlatformObject;
struct StructuredSerializeOptions;

}

namespace Web::HTML {

struct StructuredSerializeOptions;

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports
class WEB_API MessagePort final
    : public DOM::EventTarget
    , public Bindings::Transferable {
    WEB_WRAPPABLE(MessagePort, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MessagePort);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<MessagePort> create(GC::Ref<DOM::EventTarget> relevant_global_event_target);

    static void for_each_message_port(Function<void(MessagePort&)>);

    virtual ~MessagePort() override;

    JS::Object& relevant_global_object() const;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
    void entangle_with(MessagePort&);

    void disentangle();

    GC::Ptr<MessagePort> entangled_port() { return m_remote_port; }
    GC::Ptr<MessagePort const> entangled_port() const { return m_remote_port; }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, StructuredSerializeOptions const& options);
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, Bindings::StructuredSerializeOptions const& options);

    void enable();
    void start();

    void close();

    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();

    // ^Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(JS::Realm&, HTML::TransferDataEncoder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(JS::Realm&, HTML::TransferDataDecoder&) override;
    virtual HTML::TransferType primary_interface() const override { return HTML::TransferType::MessagePort; }

    void set_worker_event_target(GC::Ref<DOM::EventTarget>);

    WebIDL::ExceptionOr<void> message_port_post_message_steps(JS::Realm&, GC::Ptr<MessagePort> target_port, JS::Value message, StructuredSerializeOptions const& options);

private:
    explicit MessagePort(GC::Ref<DOM::EventTarget> relevant_global_event_target);

    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    bool is_entangled() const;

    void dispatch_pending_messages();
    void flush_pending_outgoing_messages();
    void queue_message_task(SerializedTransferRecord&&);
    void drain_transport();
    void post_message_task_steps(SerializedTransferRecord&);
    void post_port_message(SerializedTransferRecord const&);
    ErrorOr<void> send_message_on_transport(SerializedTransferRecord const&);
    void read_from_transport();

    // The HTML spec implies(!) that this is MessagePort.[[RemotePort]]
    GC::Ptr<MessagePort> m_remote_port;

    // https://html.spec.whatwg.org/multipage/web-messaging.html#has-been-shipped
    bool m_has_been_shipped { false };

    OwnPtr<IPC::Transport> m_transport;

    GC::Ref<DOM::EventTarget> m_global_event_target;
    GC::Ptr<DOM::EventTarget> m_worker_event_target;

    Vector<SerializedTransferRecord> m_pending_incoming_messages;
    Vector<SerializedTransferRecord> m_pending_outgoing_messages;
    bool m_should_shutdown_on_enable { false };
    bool m_enabled { false };
};

}

namespace Web::Bindings {

WEB_API GC::Ref<PlatformObject> message_port(JS::Realm&, GC::Ref<HTML::MessagePort>);
WEB_API HTML::MessagePort* message_port_from_value(JS::Value);
WEB_API GC::RootVector<GC::Ref<HTML::MessagePort>> message_ports_from_transferred_values(Vector<GC::Root<JS::Object>> const&);
WEB_API bool transfer_list_contains_message_port(GC::RootVector<GC::Ref<JS::Object>> const&, HTML::MessagePort const&);
WEB_API HTML::MessagePort* message_port_from_object(JS::Object&);
WEB_API void serialize_message_port_with_transfer(JS::Realm&, HTML::TransferDataEncoder&, GC::Ref<HTML::MessagePort>);

}
