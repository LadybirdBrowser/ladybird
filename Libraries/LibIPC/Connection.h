/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Queue.h>
#include <LibCore/EventReceiver.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>

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

    virtual void shutdown_with_error(Error const&);
    virtual OwnPtr<Message> try_parse_message(ReadonlyBytes, Queue<File>&) = 0;

    OwnPtr<IPC::Message> wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id);
    void wait_for_transport_to_become_readable();
    enum class PeerEOF {
        No,
        Yes
    };
    PeerEOF drain_messages_from_peer();

    void handle_messages();

    IPC::Stub& m_local_stub;

    NonnullOwnPtr<Transport> m_transport;

    Vector<NonnullOwnPtr<Message>> m_unprocessed_messages;

    u32 m_local_endpoint_magic { 0 };
};

template<typename LocalEndpoint, typename PeerEndpoint>
class Connection : public ConnectionBase {
public:
    Connection(IPC::Stub& local_stub, NonnullOwnPtr<Transport> transport)
        : ConnectionBase(local_stub, move(transport), LocalEndpoint::static_magic())
    {
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

    virtual OwnPtr<Message> try_parse_message(ReadonlyBytes bytes, Queue<File>& fds) override
    {
        auto local_message = LocalEndpoint::decode_message(bytes, fds);
        if (!local_message.is_error())
            return local_message.release_value();

        auto peer_message = PeerEndpoint::decode_message(bytes, fds);
        if (!peer_message.is_error())
            return peer_message.release_value();

        dbgln("Failed to parse IPC message:");
        dbgln("  Local endpoint error: {}", local_message.error());
        dbgln("  Peer endpoint error: {}", peer_message.error());

        return nullptr;
    }
};

}
