/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Queue.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/Thread.h>

namespace IPC {

struct Completer {
    u64 request_id { 0 };
    Function<void(OwnPtr<IPC::Message>)> promise_completer;
};

class ConnectionBase : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ConnectionBase);

    void process_messages();

public:
    virtual ~ConnectionBase() override;

    [[nodiscard]] bool is_open() const;
    ErrorOr<void> post_message(Message const&);
    ErrorOr<void> post_message(MessageBuffer);

    void shutdown();
    virtual void die() { }

    Transport& transport() { return m_transport; }

    ErrorOr<void> drain_messages_from_peer();

protected:
    explicit ConnectionBase(IPC::Stub&, Transport, u32 local_endpoint_magic);

    virtual void may_have_become_unresponsive() { }
    virtual void did_become_responsive() { }
    virtual void shutdown_with_error(Error const&);
    virtual OwnPtr<Message> try_parse_message(ReadonlyBytes, Queue<IPC::File>&) = 0;

    OwnPtr<IPC::Message> wait_for_specific_endpoint_message_impl(u64 request_id, u32 endpoint_magic, int message_id);
    void wait_for_transport_to_become_readable();
    ErrorOr<Vector<u8>> read_as_much_as_possible_from_transport_without_blocking();

    void try_parse_messages(Vector<u8> const& bytes, size_t& index);

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
    // Arbitrary inline size.
    Vector<Completer, 16> m_resolvers;
    Core::EventLoop& m_event_loop;
};

template<typename LocalEndpoint, typename PeerEndpoint>
class Connection : public ConnectionBase {
public:
    Connection(IPC::Stub& local_stub, Transport transport)
        : ConnectionBase(local_stub, move(transport), LocalEndpoint::static_magic())
    {
    }

    ~Connection() override = default;

    template<typename MessageType>
    OwnPtr<MessageType> wait_for_specific_message()
    {
        return wait_for_specific_endpoint_message<MessageType, LocalEndpoint>();
    }

    template<typename RequestType, typename... Args>
    NonnullOwnPtr<typename RequestType::ResponseType> send_sync(Args&&... args)
    {
        auto const request_id = LocalEndpoint::next_ipc_request_id();
        MUST(post_message(RequestType(request_id, forward<Args>(args)...)));
        auto response = wait_for_specific_endpoint_message<typename RequestType::ResponseType, PeerEndpoint>(request_id);
        VERIFY(response);
        return response.release_nonnull();
    }

    template<typename RequestType, typename... Args>
    NonnullRefPtr<Core::Promise<OwnPtr<typename RequestType::ResponseType>>> send(Args&&... args)
    {
        using Promise = Core::Promise<OwnPtr<typename RequestType::ResponseType>>;
        auto promise = Promise::construct();
        auto const msg = RequestType { LocalEndpoint::next_ipc_request_id(), forward<Args>(args)... };
        MUST(post_message(msg));
        m_resolvers.empend(msg.ipc_request_id(), [promise, msg_id = msg.message_id()](OwnPtr<IPC::Message> msg) {
            ASSERT(msg->message_id() == msg_id + 1 && msg->endpoint_magic() == PeerEndpoint::static_magic() && msg->ipc_request_id() == msg_id);
            promise->resolve(msg.release_nonnull<typename RequestType::ResponseType>());
        });
        return promise;
    }

    template<typename RequestType, typename... Args>
    OwnPtr<typename RequestType::ResponseType> send_sync_but_allow_failure(Args&&... args)
    {
        auto const request_id = LocalEndpoint::next_ipc_request_id();
        if (post_message(RequestType(request_id, forward<Args>(args)...)).is_error())
            return nullptr;
        return wait_for_specific_endpoint_message<typename RequestType::ResponseType, PeerEndpoint>(request_id);
    }

protected:
    template<typename MessageType, typename Endpoint>
    OwnPtr<MessageType> wait_for_specific_endpoint_message(u64 request_id)
    {
        if (auto message = wait_for_specific_endpoint_message_impl(request_id, Endpoint::static_magic(), MessageType::static_message_id()))
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
