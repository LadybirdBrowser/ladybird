/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Diagnostics.h>
#include <AK/ScopeGuard.h>
#include <AK/TemporaryChange.h>
#include <AK/Vector.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>

namespace IPC {

// NB: Drop each thread's reference when that thread exits.
#ifdef AK_COMPILER_CLANG
AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local RefPtr<SyncRequestCanceller> s_sync_request_canceller)
#else
static thread_local RefPtr<SyncRequestCanceller> s_sync_request_canceller;
#endif

void SyncRequestCanceller::install_on_this_thread()
{
    s_sync_request_canceller = *this;
}

bool SyncRequestCanceller::is_installed_on_this_thread()
{
    return s_sync_request_canceller;
}

void SyncRequestCanceller::cancel()
{
    m_cancelled.store(true, AK::MemoryOrder::memory_order_release);

    MutexLocker locker(m_mutex);
    if (m_transport_waited_on)
        m_transport_waited_on->interrupt_wait_until_readable();
}

void SyncRequestCanceller::set_transport_waited_on(Transport* transport)
{
    MutexLocker locker(m_mutex);
    m_transport_waited_on = transport;
}

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, NonnullOwnPtr<Transport> transport, u32 local_endpoint_magic)
    : m_local_stub(local_stub)
    , m_transport(move(transport))
    , m_local_endpoint_magic(local_endpoint_magic)
{
    m_transport->set_up_read_hook([this] {
        NonnullRefPtr protect = *this;
        drain_messages_from_peer();
        handle_messages();
    });
}

ConnectionBase::~ConnectionBase() = default;

bool ConnectionBase::is_open() const
{
    return m_transport->is_open();
}

ErrorOr<void> ConnectionBase::post_message(Message const& message)
{
    auto buffer = TRY(message.encode());
    return post_message(buffer);
}

ErrorOr<void> ConnectionBase::post_message(MessageBuffer& buffer)
{
    // NOTE: If this connection is being shut down, but has not yet been destroyed,
    //       the socket will be closed. Don't try to send more messages.
    if (!m_transport->is_open())
        return Error::from_string_literal("Trying to post_message during IPC shutdown");

    TRY(buffer.transfer_message(*m_transport));

    return {};
}

ErrorOr<void> ConnectionBase::post_sync_request(Message const& request)
{
    auto buffer = TRY(request.encode());
    return post_sync_request(buffer);
}

ErrorOr<void> ConnectionBase::post_sync_request(MessageBuffer& request)
{
    auto result = post_message(request);
    // NB: No response can come if the peer has closed the connection, which is the peer going away just as when a wait
    // for the response finds it gone. So, the connection learns of that, rather than hand the failure to a caller that
    // may have no way to carry on.
    if (result.is_error() && !is_open())
        did_find_transport_closed_during_sync_request();
    return result;
}

void ConnectionBase::did_find_transport_closed_during_sync_request()
{
    if (m_peer_owns_this_process) {
        did_lose_peer_during_sync_request();
        return;
    }
    // NB: The caller of the request that failed may still use the connection, which dying could let go of. The event
    // loop keeps it alive until then.
    if (!m_died)
        deferred_invoke([this] { did_lose_peer_during_sync_request(); });
}

bool ConnectionBase::send_raw_sync_request(MessageBuffer& request, u32 response_endpoint_magic, i32 response_message_id, Function<void(FfiRawMessage)> const& hand_over_response)
{
    if (post_sync_request(request).is_error())
        return false;
    auto response = wait_for_specific_endpoint_message_impl(response_endpoint_magic, response_message_id, MayCancel::Yes);
    if (!response)
        return false;
    auto outcome = static_cast<RawMessage&>(*response).hand_to_rust(hand_over_response);
    if (outcome.misbehavior.has_value()) {
        dbgln("IPC::ConnectionBase ({:p}) got a response it cannot use ({}), disconnecting.", this, *outcome.misbehavior);
        shutdown();
        return false;
    }
    return true;
}

bool ConnectionBase::send_sync_request_from_rust(void* connection, ReadonlyBytes bytes, ReadonlySpan<RawAttachment> attachments, u32 response_endpoint_magic, i32 response_message_id, void* response_sink, void (*take_response)(void* response_sink, FfiRawMessage))
{
    auto& self = *static_cast<ConnectionBase*>(connection);
    VERIFY(self.m_owner_thread_id.is_current_thread());
    auto request = adopt_encoded_message(bytes, attachments);
    return self.send_raw_sync_request(request, response_endpoint_magic, response_message_id, [&](FfiRawMessage response) {
        take_response(response_sink, response);
    });
}

void ConnectionBase::ref_from_rust(void* connection)
{
    auto& self = *static_cast<ConnectionBase*>(connection);
    VERIFY(self.m_owner_thread_id.is_current_thread());
    self.ref();
}

void ConnectionBase::unref_from_rust(void* connection)
{
    auto& self = *static_cast<ConnectionBase*>(connection);
    VERIFY(self.m_owner_thread_id.is_current_thread());
    self.unref();
}

FfiConnection ConnectionBase::to_ffi_with_one_reference()
{
    VERIFY(m_owner_thread_id.is_current_thread());
    ref();
    return {
        .connection_with_one_reference = this,
        .send_sync_request = send_sync_request_from_rust,
        .ref = ref_from_rust,
        .unref = unref_from_rust,
    };
}

void ConnectionBase::shutdown()
{
    VERIFY(m_owner_thread_id.is_current_thread());
    if (m_died)
        return;
    m_died = true;
    m_transport->close();

    m_unprocessed_messages.clear();
    for (auto batch : m_dispatching_message_batches) {
        for (auto& message : batch)
            message.clear();
    }

    die();
}

void ConnectionBase::shutdown_with_error(Error const& error)
{
    dbgln("IPC::ConnectionBase ({:p}) had an error ({}), disconnecting.", this, error);
    shutdown();
}

Vector<NonnullOwnPtr<Message>> ConnectionBase::take_unprocessed_messages(u32 endpoint_magic, i32 message_id)
{
    VERIFY(m_owner_thread_id.is_current_thread());
    Vector<NonnullOwnPtr<Message>> taken;
    // A handler can make a synchronous request while older messages still await dispatch
    // in its batch. Take those first, including batches suspended by nested event loops.
    for (auto batch : m_dispatching_message_batches) {
        for (auto& message : batch) {
            if (message && message->endpoint_magic() == endpoint_magic && message->message_id() == message_id)
                taken.append(message.release_nonnull());
        }
    }
    m_unprocessed_messages.remove_all_matching([&](auto& message) {
        if (message->endpoint_magic() != endpoint_magic || message->message_id() != message_id)
            return false;
        taken.append(message.release_nonnull());
        return true;
    });
    return taken;
}

void ConnectionBase::dispatch_pending_messages()
{
    VERIFY(m_owner_thread_id.is_current_thread());
    m_transport->wait_until_incoming_is_current();
    drain_messages_from_peer();
    handle_messages();
}

void ConnectionBase::handle_messages()
{
    VERIFY(m_owner_thread_id.is_current_thread());
    if (m_died)
        return;
    auto messages = move(m_unprocessed_messages);
    m_dispatching_message_batches.append(messages.span());
    ScopeGuard remove_batch = [&] { m_dispatching_message_batches.take_last(); };
    for (auto& message : messages) {
        if (!message)
            continue;
        auto current_message = message.release_nonnull();
        if (current_message->endpoint_magic() != m_local_endpoint_magic)
            continue;

        if (!is_open()) {
            // NB: A peer that owns this process has closed the connection, so there's nothing left to do here but shut
            // down; see set_peer_owns_this_process().
            if (m_peer_owns_this_process) {
                shutdown();
                return;
            }
            dbgln("Handling message while connection closed: {}", current_message->message_name());
        }

        auto handler_result = m_local_stub.handle(move(current_message));
        if (handler_result.is_error()) {
            dbgln("IPC::ConnectionBase::handle_messages: {}", handler_result.error());
            continue;
        }

        if (!is_open())
            continue;

        if (auto response = handler_result.release_value()) {
            if (auto post_result = post_message(*response); post_result.is_error())
                dbgln("IPC::ConnectionBase::handle_messages: {}", post_result.error());
        }
    }
}

void ConnectionBase::wait_for_transport_to_become_readable(Optional<MonotonicTime> deadline)
{
    m_transport->wait_until_readable(deadline);
}

ConnectionBase::PeerEOF ConnectionBase::drain_messages_from_peer()
{
    VERIFY(m_owner_thread_id.is_current_thread());
    if (m_died)
        return PeerEOF::Yes;
    bool parse_error = false;
    auto schedule_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
        auto bytes = raw_message.bytes.bytes();
        if (auto message = try_parse_message(raw_message.bytes, raw_message.attachments)) {
            m_unprocessed_messages.append(message.release_nonnull());
        } else {
            dbgln("Failed to parse IPC message {:hex-dump}", bytes);
            parse_error = true;
        }
    });

    if (parse_error) {
        dbgln("IPC::ConnectionBase ({:p}): Disconnecting peer after failing to parse a message", this);
        schedule_shutdown = Transport::ShouldShutdown::Yes;
    }

    // NB: Once a peer that owns this process has closed the connection, whatever it sent before closing is moot — and
    // dispatching that would run handlers on a connection that's closed under them. So, drop it all and shut down now,
    // rather than after one last dispatch; see set_peer_owns_this_process().
    if (schedule_shutdown == Transport::ShouldShutdown::Yes && m_peer_owns_this_process) {
        // NB: A wait for a sync response acts on this once it sees that no response came, and shuts down then.
        if (m_is_waiting_for_sync_response)
            m_unprocessed_messages.clear();
        else
            shutdown();
        return PeerEOF::Yes;
    }

    if (!m_unprocessed_messages.is_empty()) {
        deferred_invoke([this] {
            handle_messages();
        });
    }

    if (schedule_shutdown == Transport::ShouldShutdown::Yes) {
        deferred_invoke([this] {
            shutdown();
        });
        return PeerEOF::Yes;
    }

    return PeerEOF::No;
}

OwnPtr<IPC::Message> ConnectionBase::wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id, MayCancel may_cancel, Optional<MonotonicTime> deadline)
{
    VERIFY(m_owner_thread_id.is_current_thread());
    TemporaryChange waiting_for_sync_response { m_is_waiting_for_sync_response, true };
    bool peer_disconnected_during_wait = false;

    // NB: The canceller knows the transport before this thread looks at its flag, so it cannot slip in between that look
    //     and the wait: if it does not find the transport, this thread sees the flag.
    auto canceller = may_cancel == MayCancel::Yes ? s_sync_request_canceller : nullptr;
    if (canceller)
        canceller->set_transport_waited_on(m_transport.ptr());
    ScopeGuard forget_transport = [&] {
        if (canceller)
            canceller->set_transport_waited_on(nullptr);
    };

    auto take_matching_message = [&]() -> OwnPtr<IPC::Message> {
        for (size_t i = 0; i < m_unprocessed_messages.size(); ++i) {
            auto& message = m_unprocessed_messages[i];
            if (message->endpoint_magic() != endpoint_magic)
                continue;
            if (message->message_id() == message_id)
                return m_unprocessed_messages.take(i);
        }
        return {};
    };

    for (;;) {
        // NB: Responses have no ID, so one that a cancelled wait left behind would pass for the response to the next
        //     request of that type. A canceller stays cancelled, so nothing takes that response after this point. It
        //     is not dispatched either, as it is not for this endpoint.
        if (canceller && canceller->is_cancelled())
            return {};

        // Double check we don't already have the event waiting for us.
        // Otherwise we might end up blocked for a while for no reason.
        if (auto message = take_matching_message())
            return message;

        // NB: A transport no longer counts as open once its peer is gone or a send has failed, which can be before it has
        //     taken in everything the peer sent, the response among it.
        if (!is_open()) {
            m_transport->wait_until_incoming_is_current();
            if (drain_messages_from_peer() == PeerEOF::Yes)
                peer_disconnected_during_wait = true;
            if (canceller && canceller->is_cancelled())
                return {};
            if (auto message = take_matching_message())
                return message;
            break;
        }
        if (deadline.has_value() && MonotonicTime::now() >= *deadline)
            return {};

        wait_for_transport_to_become_readable(deadline);
        if (drain_messages_from_peer() == PeerEOF::Yes) {
            peer_disconnected_during_wait = true;
            break;
        }
    }

    // The EOF-reporting drain can deliver the awaited response in the same batch. A peer replying then exiting produces
    // that. Take the response instead of failing; the drain-scheduled deferred handle_messages/shutdown still run after-
    // wards. So, other messages from that final batch are dispatched in order before the connection close is acted on.
    if (peer_disconnected_during_wait) {
        if (auto message = take_matching_message())
            return message;
    }

    dbgln("Failed to receive message_id: {}", message_id);

    if (!m_unprocessed_messages.is_empty()) {
        dbgln("Transport shutdown with unprocessed messages left: {}", m_unprocessed_messages.size());
        for (size_t i = 0; i < m_unprocessed_messages.size(); ++i) {
            auto& message = m_unprocessed_messages[i];
            dbgln(" Message {:03} is: {:2}-{}", i, message->message_id(), message->message_name());
        }
    }

    if (peer_disconnected_during_wait) {
        // shutdown() discards any remaining queued messages. wait_for_specific_endpoint_message_impl can be entered
        // from any sync IPC call — including from inside a constructor whose members are still being initialized. (See
        // issue #9582. PageHost's constructor issues a sync IPC before ConnectionFromClient::m_page_host has been
        // assigned.) Re-entering arbitrary handlers from here can hit uninitialized state and crash. shutdown() closes
        // the transport and calls die(). That exits processes cleanly — the same as queued close_server message would.
        did_lose_peer_during_sync_request();
    } else if (!is_open()) {
        // NB: The peer may also have been found gone before the wait began — e.g. by the transport's IO thread, right
        // after the request was posted; see set_peer_owns_this_process(). A transport whose send failed is closed as
        // well, even while the peer keeps writing, and no response can come then either.
        did_find_transport_closed_during_sync_request();
    }

    return {};
}

}
