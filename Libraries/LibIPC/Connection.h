/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Queue.h>
#include <AK/ThreadID.h>
#include <LibCore/EventReceiver.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibIPC/RawMessage.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/Transport.h>

namespace IPC {

// Lets any thread end the sync requests of the thread this is installed on, on whichever connection they are. A request
// that may fail then fails, now and from then on, rather than wait for a peer that may never answer. This is for a thread
// that shares its process with others, so that it cannot just exit. Such a thread has no use for a request that cannot
// fail, which would take the whole process along, so it may not make one; see send_sync().
class SyncRequestCanceller final : public AtomicRefCounted<SyncRequestCanceller> {
public:
    static NonnullRefPtr<SyncRequestCanceller> create() { return adopt_ref(*new SyncRequestCanceller); }

    // This stays the canceller of the current thread until the thread ends.
    void install_on_this_thread();
    static bool is_installed_on_this_thread();

    void cancel();
    bool is_cancelled() const { return m_cancelled.load(AK::MemoryOrder::memory_order_acquire); }

private:
    friend class ConnectionBase;

    SyncRequestCanceller() = default;

    void set_transport_waited_on(Transport*);

    Atomic<bool> m_cancelled { false };
    Mutex m_mutex;
    Transport* m_transport_waited_on { nullptr };
};

class ConnectionBase : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ConnectionBase);

public:
    virtual ~ConnectionBase() override;

    [[nodiscard]] bool is_open() const;
    ErrorOr<void> post_message(Message const&);
    ErrorOr<void> post_message(MessageBuffer&);

    void shutdown();
    virtual void die() { }

    // Tells the connection that its peer owns this process — i.e. that the process exists only to serve that peer, and
    // that die() ends it, right away or once its event loop unwinds. Such a process has nothing left to do once that
    // peer has closed the connection. So, the connection then shuts down rather than be used any further: It drops
    // whatever the peer sent before closing, rather than dispatch that to handlers that'd find the connection closed
    // under them. And it shuts down on a sync request, rather than fail one whose caller may have no way to carry on
    // without the response.
    // Gecko/WebKit/Blink do the same for a child process's connection to its parent, each exiting the process right
    // there: Gecko SetAbortOnError, Blink TerminateSelfOnDisconnect, WebKit
    // setDidCloseOnConnectionWorkQueueCallback/setShouldExitOnSyncMessageSendFailure.
    // NB: Those all exit from their IO thread, which also ends a process whose main thread is stuck in script. This
    // acts on the connection's own thread only — so it doesn't.
    void set_peer_owns_this_process(bool value) { m_peer_owns_this_process = value; }

    Transport& transport() const { return *m_transport; }

    // Messages of one kind that have arrived and have not been dispatched yet, in
    // arrival order. A caller that asked the peer for state synchronously reads what the peer sent ahead of
    // its answer this way, without dispatching anything else out of turn.
    Vector<NonnullOwnPtr<Message>> take_unprocessed_messages(u32 endpoint_magic, i32 message_id);

    // A sync request cannot get a response, as a peer that owns this process has closed the connection. The caller of
    // send_sync() has no way to carry on, so a connection whose die() returns has to end the process here.
    virtual void did_lose_peer_during_sync_request() { shutdown(); }

    // Dispatch every message the peer has already sent on this connection, without waiting for new ones.
    void dispatch_pending_messages();

    template<typename RequestType, typename... Args>
    NonnullOwnPtr<typename RequestType::ResponseType> send_sync(Args&&... args)
    {
        // NB: A peer that dies here ends this process, and nothing can stop this wait. A thread with a canceller has to
        //     fail on its own, so all it asks for goes through send_sync_but_allow_failure().
        VERIFY(!SyncRequestCanceller::is_installed_on_this_thread());
        MUST(post_sync_request(RequestType(forward<Args>(args)...)));
        auto response = wait_for_specific_endpoint_message<typename RequestType::ResponseType>();
        VERIFY(response);
        return response.release_nonnull();
    }

    template<typename RequestType, typename... Args>
    OwnPtr<typename RequestType::ResponseType> send_sync_but_allow_failure(Args&&... args)
    {
        if (post_sync_request(RequestType(forward<Args>(args)...)).is_error())
            return nullptr;
        return wait_for_specific_endpoint_message<typename RequestType::ResponseType>(MayCancel::Yes);
    }

protected:
    enum class MayCancel {
        No,
        Yes,
    };

    template<typename MessageType>
    OwnPtr<MessageType> wait_for_specific_endpoint_message(MayCancel may_cancel = MayCancel::No)
    {
        if (auto message = wait_for_specific_endpoint_message_impl(MessageType::ENDPOINT_MAGIC, MessageType::static_message_id(), may_cancel))
            return message.template release_nonnull<MessageType>();
        return {};
    }

    explicit ConnectionBase(IPC::Stub&, NonnullOwnPtr<Transport>, u32 local_endpoint_magic);

    virtual void shutdown_with_error(Error const&);
    virtual OwnPtr<Message> try_parse_message(ReceivedMessageBytes const&, Queue<Attachment>&) = 0;

    ErrorOr<void> post_sync_request(Message const&);
    OwnPtr<IPC::Message> wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id, MayCancel = MayCancel::No);
    void wait_for_transport_to_become_readable();
    enum class PeerEOF {
        No,
        Yes
    };
    PeerEOF drain_messages_from_peer();

    void handle_messages();

    AK::ThreadID m_owner_thread_id { AK::ThreadID::current() };

    IPC::Stub& m_local_stub;

    NonnullOwnPtr<Transport> m_transport;

    Vector<OwnPtr<Message>> m_unprocessed_messages;
    Vector<Span<OwnPtr<Message>>> m_dispatching_message_batches;

    u32 m_local_endpoint_magic { 0 };

    bool m_peer_owns_this_process { false };
    bool m_is_waiting_for_sync_response { false };
    bool m_died { false };
};

template<typename LocalEndpoint, typename PeerEndpoint>
class Connection : public ConnectionBase {
public:
    Connection(IPC::Stub& local_stub, NonnullOwnPtr<Transport> transport)
        : ConnectionBase(local_stub, move(transport), LocalEndpoint::static_magic())
    {
    }

protected:
    virtual OwnPtr<Message> try_parse_message(ReceivedMessageBytes const& received_bytes, Queue<Attachment>& attachments) override
    {
        auto bytes = received_bytes.bytes();
        if (auto header = RawMessage::read_header(bytes); header.has_value() && header->endpoint_magic == LocalEndpoint::static_magic()) {
            if (auto message_name = LocalEndpoint::name_of_request_that_arrives_raw(header->message_id); message_name.has_value()) {
                Vector<Attachment> attachments_of_message;
                while (!attachments.is_empty())
                    attachments_of_message.append(attachments.dequeue());
                return make<RawMessage>(*header, *message_name, received_bytes, move(attachments_of_message));
            }
        }

        auto local_message = LocalEndpoint::decode_message(bytes, attachments);
        if (!local_message.is_error())
            return local_message.release_value();

        auto peer_message = PeerEndpoint::decode_message(bytes, attachments);
        if (!peer_message.is_error())
            return peer_message.release_value();

        dbgln("Failed to parse IPC message:");
        dbgln("  Local endpoint error: {}", local_message.error());
        dbgln("  Peer endpoint error: {}", peer_message.error());

        return nullptr;
    }
};

}
