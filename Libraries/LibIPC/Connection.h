/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
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
#include <LibIPC/Transport.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/Thread.h>

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

    Transport& transport() { return m_transport; }

protected:
    explicit ConnectionBase(IPC::Stub&, Transport, u32 local_endpoint_magic);

    virtual void may_have_become_unresponsive() { }
    virtual void did_become_responsive() { }
    virtual void shutdown_with_error(Error const&);
    virtual OwnPtr<Message> try_parse_message(ReadonlyBytes, Queue<IPC::File>&) = 0;

    OwnPtr<IPC::Message> wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id);
    void wait_for_transport_to_become_readable();
    ErrorOr<Vector<u8>> read_as_much_as_possible_from_transport_without_blocking();
    ErrorOr<void> drain_messages_from_peer();
    void try_parse_messages(Vector<u8> const& bytes, size_t& index);

    void handle_messages();

    IPC::Stub& m_local_stub;

    Transport m_transport;

    RefPtr<Core::Timer> m_responsiveness_timer;

    Vector<NonnullOwnPtr<Message>> m_unprocessed_messages;
    Queue<IPC::File> m_unprocessed_fds; // unused on Windows
    ByteBuffer m_unprocessed_bytes;

    u32 m_local_endpoint_magic { 0 };

    struct SendQueue : public AtomicRefCounted<SendQueue> {
        AK::SinglyLinkedList<MessageBuffer> messages;
        Threading::Mutex mutex;
        Threading::ConditionVariable condition { mutex };
        bool running { true };
    };

    RefPtr<Threading::Thread> m_send_thread;
    RefPtr<SendQueue> m_send_queue;
};

template<typename LocalEndpoint, typename PeerEndpoint>
class Connection : public ConnectionBase {
public:
    Connection(IPC::Stub& local_stub, Transport transport)
        : ConnectionBase(local_stub, move(transport), LocalEndpoint::static_magic())
    {
    }

    template<typename MessageType>
    OwnPtr<MessageType> wait_for_specific_message()
    {
        return wait_for_specific_endpoint_message<MessageType, LocalEndpoint>();
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

    virtual OwnPtr<Message> try_parse_message(ReadonlyBytes bytes, Queue<IPC::File>& fds) override
    {
        auto local_message = LocalEndpoint::decode_message(bytes, fds);
        if (!local_message.is_error())
            return local_message.release_value();

        auto peer_message = PeerEndpoint::decode_message(bytes, fds);
        if (!peer_message.is_error())
            return peer_message.release_value();

        return nullptr;
    }
};

}
