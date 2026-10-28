/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/Ids.h>

namespace Web::Fetch::Engine {

// Invoked outside the channel lock, possibly from the peer's thread; holds no GC state.
class WEB_API FetchByteChannelConsumerSink : public AtomicRefCounted<FetchByteChannelConsumerSink> {
public:
    virtual ~FetchByteChannelConsumerSink() = default;

    // The ticket names the arm_waiter() call this wake answers; a stale wake is rejected.
    virtual void wake(WakeTicket) = 0;
};

class WEB_API FetchByteChannelProducerSink : public AtomicRefCounted<FetchByteChannelProducerSink> {
public:
    virtual ~FetchByteChannelProducerSink() = default;

    // Grants the transport permission to deliver this many further bytes.
    virtual void grant_credit(u64 byte_count) = 0;

    virtual void consumer_cancelled() = 0;

    // Called for a delivery no honest transport could have made.
    virtual void misbehaved(StringView reason);
};

// The buffer HTTP-network fetch keeps between the network layer and a response body's stream; credit grants are
// its suspend and resume.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
class WEB_API FetchByteChannel final : public AtomicRefCounted<FetchByteChannel> {
public:
    struct Watermarks {
        u64 low { 0 };
        u64 high { 0 };
    };

    struct Open { };
    struct Closed { };
    struct Errored {
        // The error a consumer turns into an exception in its own realm.
        ByteBuffer message;
    };
    struct ConsumerCancelled { };
    struct Transferred { };
    using State = Variant<Open, Closed, Errored, ConsumerCancelled, Transferred>;

    // Snapshotted atomically with the bytes taken or peeked.
    struct TakeResult {
        Vector<Core::ImmutableBytes> taken;
        State state { Open {} };
        u64 buffered_byte_count { 0 };
    };

    [[nodiscard]] static NonnullRefPtr<FetchByteChannel> create(Watermarks, NonnullRefPtr<FetchByteChannelProducerSink>);

    // The credit the producer should hand the transport before the first write.
    [[nodiscard]] u64 initial_credit() const { return m_watermarks.high; }

    enum class WriteResult : u8 {
        Accepted,
        Rejected,
    };

    // A close or error is the transport's one ending; cancel() also overrides a Closed channel with undrained bytes.
    enum class TerminateResult : u8 {
        Terminated,
        AlreadyTerminated,
    };

    // Producer side.
    WriteResult write(Core::ImmutableBytes);
    // Copies the bytes; copies written one after another share a buffer until they are taken.
    WriteResult write_copy(ReadonlyBytes);
    TerminateResult close();
    TerminateResult error(Errored);

    // The bytes of a transport that delivers slices of one whole body. The first handed over while the channel is open
    // is kept until it errors, is cancelled or is transferred.
    void keep_whole_body(Core::ImmutableBytes);
    [[nodiscard]] Optional<Core::ImmutableBytes> whole_body() const;

    // Consumer side.
    // The consumer attaches its sink once, before any arm_waiter() call.
    void attach_consumer(NonnullRefPtr<FetchByteChannelConsumerSink>);
    [[nodiscard]] TakeResult take_up_to(u64 limit);
    [[nodiscard]] TakeResult take_all();

    // Either side may cancel; the producer hears of it unless it has already ended the body.
    TerminateResult cancel();

    // Ends an open or closed channel for a body that moves to another process, returning its undrained bytes. The
    // transport hears nothing, and later writes and endings do nothing.
    [[nodiscard]] Vector<Core::ImmutableBytes> transfer_out();

    // Returns up to limit bytes from the front of the buffer, removing none and granting no credit.
    [[nodiscard]] TakeResult peek_up_to(u64 limit);

    // A waiter fires once the channel buffers at least byte_count bytes, or ends.
    struct BufferedAtLeast {
        u64 byte_count;
    };
    // A waiter fires only once the channel ends: closes, errors, or is cancelled or transferred.
    struct EndOnly { };
    // Fires as BufferedAtLeast does, and keeps a copy of the first byte_count bytes and the state from the moment it
    // is due, for take_prefix().
    struct PrefixOf {
        u64 byte_count;
    };
    using WakeOn = Variant<BufferedAtLeast, EndOnly, PrefixOf>;

    // Each call adds a waiter that fires once, and returns the ticket its wake will carry; a wake already due fires
    // before this returns, outside the lock. Waiters due at the same time fire in the order they were added.
    WakeTicket arm_waiter(WakeOn = BufferedAtLeast { 1 });
    void disarm_waiter(WakeTicket);

    // What a PrefixOf waiter kept, once. A cancellation, a transfer or disarming its ticket drops it unread.
    [[nodiscard]] Optional<TakeResult> take_prefix(WakeTicket);

private:
    FetchByteChannel(Watermarks, NonnullRefPtr<FetchByteChannelProducerSink>);

    // Sink calls decided under the lock and made after it is released, in order.
    struct Wake {
        WakeTicket ticket { 0 };
    };
    struct GrantCredit {
        u64 byte_count { 0 };
    };
    struct ConsumerCancelledNotice { };
    struct Misbehaved {
        StringView reason;
    };
    using Notification = Variant<Wake, GrantCredit, ConsumerCancelledNotice, Misbehaved>;
    using PendingNotifications = Vector<Notification, 2>;

    [[nodiscard]] WriteResult write_bytes(Variant<Core::ImmutableBytes, ReadonlyBytes>);
    void seal_copied_tail_locked();
    [[nodiscard]] TakeResult take_locked(u64 limit, PendingNotifications&);
    [[nodiscard]] TerminateResult terminate_locked(State, PendingNotifications&);
    [[nodiscard]] bool is_due_locked(WakeOn const&) const;
    void wake_due_waiters_locked(PendingNotifications&);
    void keep_prefix_locked(WakeTicket, WakeOn const&);
    void dispatch(PendingNotifications const&);

    struct ArmedWaiter {
        WakeTicket ticket { 0 };
        WakeOn wake_on;
    };

    // A compact copy, so it holds none of the chunks' storage.
    struct KeptPrefix {
        WakeTicket ticket { 0 };
        TakeResult prefix;
    };

    Watermarks const m_watermarks;
    NonnullRefPtr<FetchByteChannelProducerSink> const m_producer_sink;
    RefPtr<FetchByteChannelConsumerSink> m_consumer_sink;

    mutable Mutex m_mutex;
    Vector<Core::ImmutableBytes> m_chunks;
    // The write_copy() bytes that follow the last chunk.
    ByteBuffer m_copied_tail;
    u64 m_buffered_byte_count { 0 };
    u64 m_outstanding_transport_credit { 0 };
    State m_state { Open {} };
    bool m_transport_ended { false };
    Optional<Core::ImmutableBytes> m_whole_body;
    Vector<ArmedWaiter, 2> m_armed_waiters;
    Vector<KeptPrefix, 1> m_kept_prefixes;
    u64 m_next_ticket { 1 };
};

}
