/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Forward.h>
#include <AK/Queue.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace IPC {

class ConnectionBase : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ConnectionBase);

public:
    virtual ~ConnectionBase() override;

    [[nodiscard]] bool is_open() const;
    ErrorOr<void> post_message(Message const&);
    ErrorOr<void> post_message(MessageBuffer);

    void shutdown();
    virtual void die() { }

    Transport& transport() const { return *m_transport; }

protected:
    explicit ConnectionBase(IPC::Stub&, NonnullOwnPtr<Transport>, u32 local_endpoint_magic);

    // Must be called after setting up the message decoder
    void initialize_messaging();

    virtual void shutdown_with_error(Error const&);

    OwnPtr<IPC::Message> wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id);

    void handle_messages();

    // Called from I/O thread when a message is decoded
    void on_message_received(NonnullOwnPtr<IPC::Message> message);
    void on_peer_closed();

    IPC::Stub& m_local_stub;

    NonnullOwnPtr<Transport> m_transport;

    Threading::Mutex m_unprocessed_messages_mutex;
    Threading::ConditionVariable m_unprocessed_messages_cv { m_unprocessed_messages_mutex };
    Vector<NonnullOwnPtr<Message>> m_unprocessed_messages;

    RefPtr<Core::WeakEventLoopReference> m_event_loop;

    Atomic<bool> m_peer_closed { false };

    u32 m_local_endpoint_magic { 0 };
};

template<typename LocalEndpoint, typename PeerEndpoint>
class Connection : public ConnectionBase {
public:
    Connection(IPC::Stub& local_stub, NonnullOwnPtr<Transport> transport)
        : ConnectionBase(local_stub, move(transport), LocalEndpoint::static_magic())
    {
        // Set up message handler first so we're ready to receive
        initialize_messaging();

        // Then set up decoder
        m_transport->set_message_decoder([](ReadonlyBytes bytes, Queue<File>& fds) -> OwnPtr<IPC::Message> {
            auto local = LocalEndpoint::decode_message(bytes, fds);
            if (!local.is_error())
                return local.release_value();

            auto peer = PeerEndpoint::decode_message(bytes, fds);
            if (!peer.is_error())
                return peer.release_value();

            dbgln("Failed to parse IPC message:");
            dbgln("  Local endpoint error: {}", local.error());
            dbgln("  Peer endpoint error: {}", peer.error());

            return nullptr;
        });

        // Now that handler and decoder are set up, start receiving messages
        m_transport->start();
    }

    template<typename RequestType, typename... Args>
    NonnullOwnPtr<typename RequestType::ResponseType> send_sync(Args&&... args)
    {
        MUST(post_message(RequestType(forward<Args>(args)...)));
        auto response = wait_for_specific_endpoint_message<typename RequestType::ResponseType, PeerEndpoint>();
        VERIFY(response);
        return response.release_nonnull();
    }

    template<typename RequestType, typename... Args>
    OwnPtr<typename RequestType::ResponseType> send_sync_but_allow_failure(Args&&... args)
    {
        if (post_message(RequestType(forward<Args>(args)...)).is_error())
            return nullptr;
        return wait_for_specific_endpoint_message<typename RequestType::ResponseType, PeerEndpoint>();
    }

protected:
    template<typename MessageType, typename Endpoint>
    OwnPtr<MessageType> wait_for_specific_endpoint_message()
    {
        if (auto message = wait_for_specific_endpoint_message_impl(Endpoint::static_magic(), MessageType::static_message_id()))
            return message.template release_nonnull<MessageType>();
        return {};
    }
};

}
