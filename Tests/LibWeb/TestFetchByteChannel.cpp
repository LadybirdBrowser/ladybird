/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace {

using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::FetchByteChannelConsumerSink;
using Web::Fetch::Engine::FetchByteChannelProducerSink;
using Web::Fetch::Engine::WakeTicket;
using TerminateResult = FetchByteChannel::TerminateResult;
using WriteResult = FetchByteChannel::WriteResult;
using Closed = FetchByteChannel::Closed;
using ConsumerCancelled = FetchByteChannel::ConsumerCancelled;
using Errored = FetchByteChannel::Errored;
using Open = FetchByteChannel::Open;
using Transferred = FetchByteChannel::Transferred;

struct SinkEvent {
    enum class Kind : u8 {
        Wake,
        CreditGrant,
        ConsumerCancelled,
    };
    Kind kind;
    u64 value { 0 };
};

class RecordingConsumerSink final : public FetchByteChannelConsumerSink {
public:
    explicit RecordingConsumerSink(Vector<SinkEvent>& events)
        : m_events(events)
    {
    }

    virtual void wake(WakeTicket ticket) override
    {
        m_events.append({ SinkEvent::Kind::Wake, ticket.value() });
        if (on_wake)
            on_wake(ticket);
    }

    Function<void(WakeTicket)> on_wake;

private:
    Vector<SinkEvent>& m_events;
};

class RecordingProducerSink final : public FetchByteChannelProducerSink {
public:
    explicit RecordingProducerSink(Vector<SinkEvent>& events)
        : m_events(events)
    {
    }

    virtual void grant_credit(u64 byte_count) override
    {
        m_events.append({ SinkEvent::Kind::CreditGrant, byte_count });
        if (on_grant_credit)
            on_grant_credit(byte_count);
    }

    virtual void consumer_cancelled() override
    {
        m_events.append({ SinkEvent::Kind::ConsumerCancelled });
    }

    Function<void(u64)> on_grant_credit;

private:
    Vector<SinkEvent>& m_events;
};

struct ChannelHarness {
    AK_ALLOC_WITH_KMALLOC;

    explicit ChannelHarness(FetchByteChannel::Watermarks watermarks = { .low = 16, .high = 64 })
        : consumer_sink(adopt_ref(*new RecordingConsumerSink(events)))
        , producer_sink(adopt_ref(*new RecordingProducerSink(events)))
        , channel(FetchByteChannel::create(watermarks, *producer_sink))
    {
        channel->attach_consumer(*consumer_sink);
    }

    Vector<SinkEvent> take_events() { return move(events); }

    Vector<SinkEvent> events;
    NonnullRefPtr<RecordingConsumerSink> consumer_sink;
    NonnullRefPtr<RecordingProducerSink> producer_sink;
    NonnullRefPtr<FetchByteChannel> channel;
};

struct LatestTicketSink final : FetchByteChannelConsumerSink {
    virtual void wake(WakeTicket ticket) override { latest.store(ticket.value(), AK::MemoryOrder::memory_order_release); }
    Atomic<u64> latest { 0 };
};

struct CountingProducerSink final : FetchByteChannelProducerSink {
    virtual void grant_credit(u64 byte_count) override { credit.fetch_add(byte_count, AK::MemoryOrder::memory_order_acq_rel); }
    virtual void consumer_cancelled() override { cancellations.fetch_add(1, AK::MemoryOrder::memory_order_acq_rel); }
    Atomic<u64> credit { 0 };
    Atomic<u64> cancellations { 0 };
};

// Counts the wakes each ticket receives, from any thread.
struct EveryTicketSink final : FetchByteChannelConsumerSink {
    virtual void wake(WakeTicket ticket) override
    {
        MutexLocker locker { mutex };
        ++wakes.ensure(ticket.value(), [] { return 0u; });
    }

    size_t times_woken(WakeTicket ticket)
    {
        MutexLocker locker { mutex };
        return wakes.get(ticket.value()).value_or(0);
    }

    Mutex mutex;
    HashMap<u64, size_t> wakes;
};

Core::ImmutableBytes bytes_of(StringView text)
{
    return MUST(Core::ImmutableBytes::copy(text.bytes()));
}

// Spins until the condition holds, or returns false once a minute has passed.
template<typename Condition>
bool spin_until_or_timeout(Condition condition)
{
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(60);
    for (u64 spins = 0; !condition(); ++spins) {
        AK::atomic_pause();
        if ((spins % 4096) == 0 && MonotonicTime::now() > deadline)
            return false;
    }
    return true;
}

ByteString flatten(Vector<Core::ImmutableBytes> const& taken_bytes)
{
    ByteBuffer flat;
    for (auto const& taken : taken_bytes)
        flat.append(taken.bytes());
    return ByteString { flat.bytes() };
}

ByteString flatten(FetchByteChannel::TakeResult const& result)
{
    return flatten(result.taken);
}

}

TEST_CASE(write_take_ordering_and_chunk_integrity)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("hello "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("byte "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("channel"sv)), WriteResult::Accepted);

    auto result = harness.channel->take_all();
    EXPECT_EQ(flatten(result), "hello byte channel"sv);
    EXPECT(result.state.has<Open>());
    EXPECT_EQ(result.buffered_byte_count, 0u);
}

TEST_CASE(copied_writes_share_one_chunk)
{
    ChannelHarness harness;
    for (auto byte : "copied"sv)
        EXPECT_EQ(harness.channel->write_copy({ &byte, 1 }), WriteResult::Accepted);

    auto result = harness.channel->take_all();
    EXPECT_EQ(result.taken.size(), 1u);
    EXPECT_EQ(flatten(result), "copied"sv);
}

TEST_CASE(copied_and_adopted_writes_keep_their_order)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write_copy("one "sv.bytes()), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("two "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write_copy("three"sv.bytes()), WriteResult::Accepted);

    auto first = harness.channel->take_up_to(6);
    EXPECT_EQ(flatten(first), "one tw"sv);
    EXPECT_EQ(first.buffered_byte_count, 7u);

    // Copies written after a take start a new shared chunk.
    EXPECT_EQ(harness.channel->write_copy(" four"sv.bytes()), WriteResult::Accepted);
    auto rest = harness.channel->take_all();
    EXPECT_EQ(flatten(rest), "o three four"sv);
}

TEST_CASE(copied_writes_spend_credit_like_adopted_ones)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };
    auto credit_worth = MUST(String::repeated('x', 64));
    EXPECT_EQ(harness.channel->write_copy(credit_worth.bytes()), WriteResult::Accepted);
    EXPECT_DEATH("the transport delivers only what it was granted", (void)harness.channel->write_copy("x"sv.bytes()));
}

TEST_CASE(a_channel_needs_a_positive_low_watermark)
{
    Vector<SinkEvent> events;
    auto producer_sink = adopt_ref(*new RecordingProducerSink(events));
    EXPECT_DEATH("a channel without a low watermark never refills its credit", (void)FetchByteChannel::create({ .low = 0, .high = 64 }, *producer_sink));
}

TEST_CASE(take_up_to_leaves_correctly_offset_remainder)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("0123456789"sv)), WriteResult::Accepted);

    auto first = harness.channel->take_up_to(4);
    EXPECT_EQ(flatten(first), "0123"sv);
    EXPECT_EQ(first.buffered_byte_count, 6u);

    auto second = harness.channel->take_up_to(3);
    EXPECT_EQ(flatten(second), "456"sv);
    EXPECT_EQ(second.buffered_byte_count, 3u);

    auto rest = harness.channel->take_all();
    EXPECT_EQ(flatten(rest), "789"sv);
    EXPECT_EQ(rest.buffered_byte_count, 0u);
}

TEST_CASE(take_up_to_spanning_multiple_chunks)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("abc"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("def"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("ghi"sv)), WriteResult::Accepted);

    auto result = harness.channel->take_up_to(5);
    EXPECT_EQ(flatten(result), "abcde"sv);
    EXPECT_EQ(result.buffered_byte_count, 4u);
}

TEST_CASE(credit_accounting_with_batched_replenishment)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };
    EXPECT_EQ(harness.channel->initial_credit(), 64u);

    // Writes consume transport credit; nothing is replenished while the buffer sits at or
    // above the low watermark.
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 40)))), WriteResult::Accepted);
    auto result = harness.channel->take_up_to(20);
    EXPECT_EQ(result.buffered_byte_count, 20u);
    EXPECT(harness.take_events().is_empty());

    // Draining below low replenishes back up to a high watermark's worth of
    // buffered-plus-outstanding bytes in one batch.
    result = harness.channel->take_up_to(10);
    EXPECT_EQ(result.buffered_byte_count, 10u);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::CreditGrant);
    EXPECT_EQ(events[0].value, 30u); // 64 high - 10 buffered - 24 outstanding

    // No further grant until the buffer crosses the low watermark again.
    result = harness.channel->take_up_to(1);
    EXPECT_EQ(result.buffered_byte_count, 9u);
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 1u);
}

TEST_CASE(exact_watermark_edge_transitions)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };

    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 64)))), WriteResult::Accepted);
    (void)harness.take_events();

    // Draining to exactly the low watermark does not replenish; below it does.
    auto result = harness.channel->take_up_to(48);
    EXPECT_EQ(result.buffered_byte_count, 16u);
    EXPECT(harness.take_events().is_empty());

    result = harness.channel->take_up_to(1);
    EXPECT_EQ(result.buffered_byte_count, 15u);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::CreditGrant);
    EXPECT_EQ(events[0].value, 49u);
}

TEST_CASE(a_write_beyond_the_outstanding_credit_dies)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };

    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 64)))), WriteResult::Accepted);
    EXPECT_DEATH("the transport delivers only what it was granted", (void)harness.channel->write(bytes_of("x"sv)));
}

TEST_CASE(a_write_after_the_transport_ended_the_body_dies)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    EXPECT_DEATH("the transport already ended the body", (void)harness.channel->write(bytes_of("late"sv)));
}

TEST_CASE(ending_the_body_twice_dies)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->error({}), TerminateResult::Terminated);
    EXPECT_DEATH("the transport already ended the body", (void)harness.channel->close());
}

TEST_CASE(a_write_after_consumer_cancellation_is_dropped)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
    EXPECT_EQ(harness.channel->write(bytes_of("in flight"sv)), WriteResult::Rejected);
    EXPECT_EQ(harness.channel->close(), TerminateResult::AlreadyTerminated);
}

TEST_CASE(a_zero_byte_threshold_dies)
{
    ChannelHarness harness;
    EXPECT_DEATH("a threshold counts at least one byte", (void)harness.channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 0 }));
}

TEST_CASE(arming_before_a_consumer_attaches_dies)
{
    Vector<SinkEvent> events;
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new RecordingProducerSink(events)));
    EXPECT_DEATH("a waiter needs a consumer to wake", (void)channel->arm_waiter());
}

TEST_CASE(attaching_a_second_consumer_dies)
{
    ChannelHarness harness;
    EXPECT_DEATH("a channel has one consumer", harness.channel->attach_consumer(adopt_ref(*new RecordingConsumerSink(harness.events))));
}

TEST_CASE(a_channel_buffers_and_ends_before_its_consumer_attaches)
{
    Vector<SinkEvent> events;
    auto producer_sink = adopt_ref(*new RecordingProducerSink(events));
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, producer_sink);
    EXPECT_EQ(channel->write(bytes_of("early"sv)), WriteResult::Accepted);
    EXPECT_EQ(channel->close(), TerminateResult::Terminated);

    channel->attach_consumer(adopt_ref(*new RecordingConsumerSink(events)));
    auto ticket = channel->arm_waiter(FetchByteChannel::EndOnly {});
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
    EXPECT_EQ(events[0].value, ticket.value());

    auto result = channel->take_all();
    EXPECT_EQ(flatten(result), "early"sv);
    EXPECT(result.state.has<Closed>());
}

TEST_CASE(armed_waiter_fires_on_write_and_arms_late)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter();
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(harness.channel->write(bytes_of("x"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
    EXPECT_EQ(events[0].value, ticket.value());

    // An arm_waiter() call with bytes already pending fires at once, with the fresh ticket.
    auto late_ticket = harness.channel->arm_waiter();
    EXPECT(late_ticket != ticket);
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, late_ticket.value());
}

TEST_CASE(waiter_is_one_shot_and_disarmable)
{
    ChannelHarness harness;

    (void)harness.channel->arm_waiter();
    EXPECT_EQ(harness.channel->write(bytes_of("a"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("b"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.take_events().size(), 1u);

    (void)harness.channel->take_all();
    (void)harness.take_events();

    auto ticket = harness.channel->arm_waiter();
    harness.channel->disarm_waiter(ticket);
    EXPECT_EQ(harness.channel->write(bytes_of("c"sv)), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());
}

TEST_CASE(a_prefix_waiter_keeps_its_bytes_through_a_later_error)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 4 });
    EXPECT_EQ(harness.channel->write(bytes_of("head"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("body"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) }), TerminateResult::Terminated);

    // The error empties the channel, while the waiter keeps what it saw the moment it was due.
    EXPECT(harness.channel->take_all().state.has<Errored>());
    auto kept = harness.channel->take_prefix(ticket);
    EXPECT(kept.has_value());
    EXPECT_EQ(flatten(*kept), "head"sv);
    EXPECT(kept->state.has<Open>());
}

TEST_CASE(a_prefix_waiter_keeps_every_byte_of_a_short_close)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 16 });
    EXPECT_EQ(harness.channel->write(bytes_of("short"sv)), WriteResult::Accepted);
    EXPECT(!harness.channel->take_prefix(ticket).has_value());
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    auto kept = harness.channel->take_prefix(ticket);
    EXPECT(kept.has_value());
    EXPECT_EQ(flatten(*kept), "short"sv);
    EXPECT(kept->state.has<Closed>());
}

TEST_CASE(a_prefix_waiter_keeps_no_bytes_of_an_early_error)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 16 });
    EXPECT_EQ(harness.channel->write(bytes_of("partial"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) }), TerminateResult::Terminated);

    auto kept = harness.channel->take_prefix(ticket);
    EXPECT(kept.has_value());
    EXPECT(kept->taken.is_empty());
    EXPECT(kept->state.has<Errored>());
}

TEST_CASE(a_prefix_waiter_armed_with_its_bytes_buffered_keeps_them_at_once)
{
    ChannelHarness harness;

    EXPECT_EQ(harness.channel->write(bytes_of("already here"sv)), WriteResult::Accepted);
    (void)harness.take_events();
    auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 7 });
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, ticket.value());

    auto kept = harness.channel->take_prefix(ticket);
    EXPECT(kept.has_value());
    EXPECT_EQ(flatten(*kept), "already"sv);
}

TEST_CASE(a_kept_prefix_is_taken_once)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 1 });
    EXPECT_EQ(harness.channel->write(bytes_of("x"sv)), WriteResult::Accepted);
    EXPECT(harness.channel->take_prefix(ticket).has_value());
    EXPECT(!harness.channel->take_prefix(ticket).has_value());

    // Another waiter's ticket finds none.
    auto plain_ticket = harness.channel->arm_waiter();
    EXPECT(!harness.channel->take_prefix(plain_ticket).has_value());
}

TEST_CASE(disarming_cancelling_or_transferring_drops_a_kept_prefix)
{
    for (int ending = 0; ending < 3; ++ending) {
        ChannelHarness harness;
        auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 4 });
        EXPECT_EQ(harness.channel->write(bytes_of("head"sv)), WriteResult::Accepted);
        if (ending == 0)
            harness.channel->disarm_waiter(ticket);
        else if (ending == 1)
            (void)harness.channel->cancel();
        else
            (void)harness.channel->transfer_out();
        EXPECT(!harness.channel->take_prefix(ticket).has_value());
    }
}

TEST_CASE(a_prefix_waiter_cancelled_or_transferred_before_its_bytes_keeps_nothing)
{
    for (bool cancel : { true, false }) {
        ChannelHarness harness;
        auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 16 });
        EXPECT_EQ(harness.channel->write(bytes_of("some"sv)), WriteResult::Accepted);
        (void)harness.take_events();
        if (cancel)
            (void)harness.channel->cancel();
        else
            (void)harness.channel->transfer_out();

        // The ending wakes the waiter, which keeps nothing.
        bool woken = false;
        for (auto const& event : harness.take_events())
            woken |= event.kind == SinkEvent::Kind::Wake && event.value == ticket.value();
        EXPECT(woken);
        EXPECT(!harness.channel->take_prefix(ticket).has_value());
    }
}

TEST_CASE(keeping_and_taking_a_prefix_leaves_the_bytes_and_credit_alone)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter(FetchByteChannel::PrefixOf { 4 });
    EXPECT_EQ(harness.channel->write(bytes_of("headbody"sv)), WriteResult::Accepted);
    (void)harness.take_events();
    EXPECT_EQ(flatten(*harness.channel->take_prefix(ticket)), "head"sv);

    // Neither keeping nor taking the prefix granted credit or removed a byte.
    EXPECT(harness.take_events().is_empty());
    EXPECT_EQ(flatten(harness.channel->peek_up_to(64)), "headbody"sv);
}

TEST_CASE(a_drained_close_leaves_nothing_to_cancel)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);

    auto result = harness.channel->take_all();
    EXPECT(result.state.has<Closed>());
}

TEST_CASE(cancellation_supersedes_an_undrained_close)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("still buffered"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    // A cancellation discards the bytes a closed channel still holds. The transport has ended the body and hears
    // nothing.
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
    EXPECT(harness.take_events().is_empty());

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT(result.state.has<ConsumerCancelled>());
    EXPECT_EQ(result.buffered_byte_count, 0u);

    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);
}

TEST_CASE(cancellation_after_error_changes_nothing)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("junk"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->error({}), TerminateResult::Terminated);

    // The error already discarded the buffer and settled the consumer.
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);
    auto result = harness.channel->take_all();
    EXPECT(result.state.has<Errored>());
}

TEST_CASE(close_preserves_buffered_bytes_until_drained)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("tail"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    auto result = harness.channel->take_all();
    EXPECT_EQ(flatten(result), "tail"sv);
    EXPECT(result.state.has<Closed>());
    EXPECT_EQ(result.buffered_byte_count, 0u);
}

TEST_CASE(error_discards_buffered_bytes_immediately)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("junk"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write_copy("copied junk"sv.bytes()), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->error({ .message = MUST(ByteBuffer::copy("connection reset"sv.bytes())) }), TerminateResult::Terminated);

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT_EQ(result.buffered_byte_count, 0u);
    EXPECT(result.state.has<Errored>());
    EXPECT_EQ(StringView { result.state.get<Errored>().message }, "connection reset"sv);
}

TEST_CASE(every_ending_wakes_a_waiter)
{
    enum class Ending : u8 {
        Close,
        Error,
        Cancel,
        Transfer,
    };
    for (auto ending : { Ending::Close, Ending::Error, Ending::Cancel, Ending::Transfer }) {
        ChannelHarness harness;
        auto ticket = harness.channel->arm_waiter();

        switch (ending) {
        case Ending::Close:
            EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
            break;
        case Ending::Error:
            EXPECT_EQ(harness.channel->error({}), TerminateResult::Terminated);
            break;
        case Ending::Cancel:
            EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
            break;
        case Ending::Transfer:
            EXPECT(harness.channel->transfer_out().is_empty());
            break;
        }

        auto events = harness.take_events();
        EXPECT(!events.is_empty());
        EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
        EXPECT_EQ(events[0].value, ticket.value());
    }
}

TEST_CASE(cancellation_notifies_the_producer)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("in flight"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);

    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::ConsumerCancelled);

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT(result.state.has<ConsumerCancelled>());
}

TEST_CASE(transfer_out_returns_the_undrained_bytes)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("first "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("second "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("third"sv)), WriteResult::Accepted);
    EXPECT_EQ(flatten(harness.channel->take_up_to(3)), "fir"sv);
    (void)harness.take_events();

    EXPECT_EQ(flatten(harness.channel->transfer_out()), "st second third"sv);
    EXPECT(harness.take_events().is_empty());

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT(result.state.has<Transferred>());
    EXPECT_EQ(result.buffered_byte_count, 0u);
}

TEST_CASE(transfer_out_of_an_undrained_close_returns_its_tail)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("tail"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    EXPECT_EQ(flatten(harness.channel->transfer_out()), "tail"sv);
    EXPECT(harness.channel->take_all().state.has<Transferred>());
}

TEST_CASE(transfer_out_returns_copied_bytes_in_order)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("adopted "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write_copy("copied"sv.bytes()), WriteResult::Accepted);

    EXPECT_EQ(flatten(harness.channel->transfer_out()), "adopted copied"sv);
}

TEST_CASE(a_transferred_channel_tells_neither_side)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("moving"sv)), WriteResult::Accepted);
    (void)harness.take_events();
    EXPECT_EQ(flatten(harness.channel->transfer_out()), "moving"sv);

    EXPECT_EQ(harness.channel->write(bytes_of("late"sv)), WriteResult::Rejected);
    EXPECT_EQ(harness.channel->close(), TerminateResult::AlreadyTerminated);
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);
    EXPECT(harness.channel->transfer_out().is_empty());

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT(result.state.has<Transferred>());

    // Neither a consumer cancellation nor credit reaches the transport.
    EXPECT(harness.take_events().is_empty());
}

TEST_CASE(an_error_after_a_transfer_tells_neither_side)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("moving"sv)), WriteResult::Accepted);
    (void)harness.take_events();
    (void)harness.channel->transfer_out();

    EXPECT_EQ(harness.channel->error({}), TerminateResult::AlreadyTerminated);
    EXPECT(harness.channel->take_all().state.has<Transferred>());
    EXPECT(harness.take_events().is_empty());
}

TEST_CASE(an_errored_or_cancelled_channel_transfers_nothing)
{
    {
        ChannelHarness harness;
        EXPECT_EQ(harness.channel->write(bytes_of("junk"sv)), WriteResult::Accepted);
        EXPECT_EQ(harness.channel->error({}), TerminateResult::Terminated);
        EXPECT(harness.channel->transfer_out().is_empty());
        EXPECT(harness.channel->take_all().state.has<Errored>());
    }
    {
        ChannelHarness harness;
        EXPECT_EQ(harness.channel->write(bytes_of("junk"sv)), WriteResult::Accepted);
        EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
        EXPECT(harness.channel->transfer_out().is_empty());
        EXPECT(harness.channel->take_all().state.has<ConsumerCancelled>());
    }
}

TEST_CASE(no_credit_replenishment_after_terminal)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 40)))), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    (void)harness.take_events();

    auto result = harness.channel->take_all();
    EXPECT(result.state.has<Closed>());
    EXPECT(harness.take_events().is_empty());
}

TEST_CASE(sink_reentrancy_does_not_deadlock)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };

    harness.consumer_sink->on_wake = [&](WakeTicket) {
        auto result = harness.channel->take_all();
        EXPECT(!result.taken.is_empty());
    };
    (void)harness.channel->arm_waiter();
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 40)))), WriteResult::Accepted);

    harness.consumer_sink->on_wake = nullptr;
    harness.producer_sink->on_grant_credit = [&](u64) {
        EXPECT_EQ(harness.channel->write(bytes_of("reentrant"sv)), WriteResult::Accepted);
    };
    // Draining below the low watermark grants credit, and the grant hook writes back into the channel.
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('y', 40)))), WriteResult::Accepted);
    (void)harness.channel->take_all();
    auto result = harness.channel->take_all();
    EXPECT_EQ(flatten(result), "reentrant"sv);
}

TEST_CASE(concurrent_writer_never_loses_a_wakeup)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto consumer_sink = adopt_ref(*new LatestTicketSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto producer_sink = adopt_ref(*new CountingProducerSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *producer_sink);
    channel->attach_consumer(*consumer_sink);
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> timed_out { false };
    producer_sink->credit.store(channel->initial_credit(), AK::MemoryOrder::memory_order_release);

    // The writer delivers only what it was granted.
    static constexpr u64 write_count = 20'000;
    auto writer = Threading::Thread::construct("channel writer"sv, [&channel, &producer_sink, &timed_out]() -> intptr_t {
        for (u64 i = 0; i < write_count; ++i) {
            auto has_credit = spin_until_or_timeout([&] {
                return producer_sink->credit.load(AK::MemoryOrder::memory_order_acquire) > 0 || timed_out.load();
            });
            if (!has_credit || timed_out.load()) {
                timed_out.store(true);
                return 1;
            }
            producer_sink->credit.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel);
            (void)channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes())));
        }
        (void)channel->close();
        return 0;
    });
    writer->start();

    // Every arm_waiter() call is owed exactly one wake carrying its own ticket, fired immediately when bytes or an
    // ending are already pending, from the writer's thread otherwise.
    u64 total_bytes_taken = 0;
    for (;;) {
        auto ticket = channel->arm_waiter();
        auto woken = spin_until_or_timeout([&] {
            return consumer_sink->latest.load(AK::MemoryOrder::memory_order_acquire) >= ticket.value() || timed_out.load();
        });
        if (!woken || timed_out.load()) {
            timed_out.store(true);
            warnln("No wake arrived for ticket {}", ticket.value());
            break;
        }
        auto result = channel->take_all();
        for (auto const& taken : result.taken)
            total_bytes_taken += taken.size();
        if (!result.state.has<Open>() && result.buffered_byte_count == 0)
            break;
    }
    MUST(writer->join());
    EXPECT(!timed_out.load());
    EXPECT_EQ(total_bytes_taken, write_count);
}

TEST_CASE(concurrent_writer_wakes_each_of_several_waiters_once)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto consumer_sink = adopt_ref(*new EveryTicketSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto producer_sink = adopt_ref(*new CountingProducerSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *producer_sink);
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> timed_out { false };
    producer_sink->credit.store(channel->initial_credit(), AK::MemoryOrder::memory_order_release);

    static constexpr u64 write_count = 20'000;
    auto writer = Threading::Thread::construct("channel writer"sv, [&channel, &producer_sink, &timed_out]() -> intptr_t {
        for (u64 i = 0; i < write_count; ++i) {
            auto has_credit = spin_until_or_timeout([&] {
                return producer_sink->credit.load(AK::MemoryOrder::memory_order_acquire) > 0 || timed_out.load();
            });
            if (!has_credit || timed_out.load()) {
                timed_out.store(true);
                return 1;
            }
            producer_sink->credit.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel);
            (void)channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes())));
        }
        (void)channel->close();
        return 0;
    });
    writer->start();

    // The consumer attaches while the writer runs, once a peek shows its first byte has arrived.
    auto writing = spin_until_or_timeout([&] { return !channel->peek_up_to(1).taken.is_empty() || timed_out.load(); });
    EXPECT(writing);
    channel->attach_consumer(*consumer_sink);

    // Each round arms several waiters at once and disarms one of them while the writer decides which are due.
    auto end_ticket = channel->arm_waiter(FetchByteChannel::EndOnly {});
    Vector<WakeTicket> owed;
    Vector<WakeTicket> prefixes;
    Vector<WakeTicket> disarmed;
    u64 total_bytes_taken = 0;
    for (;;) {
        auto some = channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 1 });
        owed.append(some);
        owed.append(channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 32 }));
        prefixes.append(channel->arm_waiter(FetchByteChannel::PrefixOf { 8 }));
        owed.append(prefixes.last());
        auto unwanted = channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 48 });
        channel->disarm_waiter(unwanted);
        disarmed.append(unwanted);

        auto woken = spin_until_or_timeout([&] { return consumer_sink->times_woken(some) > 0 || timed_out.load(); });
        if (!woken || timed_out.load()) {
            timed_out.store(true);
            warnln("No wake arrived for ticket {}", some.value());
            break;
        }

        // Only this thread takes, so a peek is a prefix of the take that follows it.
        auto peeked = flatten(channel->peek_up_to(4));
        auto result = channel->take_all();
        auto taken = flatten(result);
        EXPECT(taken.starts_with(peeked));
        total_bytes_taken += taken.length();
        if (!result.state.has<Open>() && result.buffered_byte_count == 0)
            break;
    }
    MUST(writer->join());

    // An armed waiter wakes exactly once, by its bytes or by the ending; a disarmed one at most once, for a wake decided
    // before it was disarmed.
    EXPECT(!timed_out.load());
    EXPECT_EQ(total_bytes_taken, write_count);
    EXPECT_EQ(consumer_sink->times_woken(end_ticket), 1u);
    for (auto ticket : owed)
        EXPECT_EQ(consumer_sink->times_woken(ticket), 1u);
    for (auto ticket : disarmed)
        EXPECT(consumer_sink->times_woken(ticket) <= 1);

    // Each prefix waiter kept at most its limit, from an open or closed channel.
    for (auto ticket : prefixes) {
        auto kept = channel->take_prefix(ticket);
        EXPECT(kept.has_value());
        EXPECT(flatten(*kept).length() <= 8);
        EXPECT(kept->state.has<Open>() || kept->state.has<Closed>());
    }
}

TEST_CASE(concurrent_cancellation_rejects_every_later_write)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto consumer_sink = adopt_ref(*new LatestTicketSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto producer_sink = adopt_ref(*new CountingProducerSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *producer_sink);
    channel->attach_consumer(*consumer_sink);
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> timed_out { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> accepted_after_rejection { 0 };
    producer_sink->credit.store(channel->initial_credit(), AK::MemoryOrder::memory_order_release);

    // The writer writes until a write is rejected, then checks that the next ones are rejected too.
    auto writer = Threading::Thread::construct("channel writer"sv, [&channel, &producer_sink, &timed_out, &accepted_after_rejection]() -> intptr_t {
        for (;;) {
            auto may_write = spin_until_or_timeout([&] {
                return producer_sink->credit.load(AK::MemoryOrder::memory_order_acquire) > 0 || producer_sink->cancellations.load() > 0;
            });
            if (!may_write) {
                timed_out.store(true);
                return 1;
            }
            if (producer_sink->cancellations.load() == 0)
                producer_sink->credit.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel);
            if (channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes()))) == WriteResult::Rejected)
                break;
        }
        for (int i = 0; i < 16; ++i) {
            if (channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes()))) == WriteResult::Accepted)
                accepted_after_rejection.fetch_add(1);
        }
        return 0;
    });
    writer->start();

    // The consumer drains a while, then cancels while the writer is mid-stream.
    u64 total_bytes_taken = 0;
    while (total_bytes_taken < 5'000) {
        auto ticket = channel->arm_waiter();
        auto woken = spin_until_or_timeout([&] {
            return consumer_sink->latest.load(AK::MemoryOrder::memory_order_acquire) >= ticket.value();
        });
        if (!woken) {
            timed_out.store(true);
            warnln("No wake arrived for ticket {}", ticket.value());
            break;
        }
        for (auto const& taken : channel->take_all().taken)
            total_bytes_taken += taken.size();
    }
    EXPECT_EQ(channel->cancel(), TerminateResult::Terminated);

    MUST(writer->join());
    EXPECT(!timed_out.load());
    EXPECT_EQ(accepted_after_rejection.load(), 0u);
    EXPECT_EQ(producer_sink->cancellations.load(), 1u);
    EXPECT(channel->take_all().state.has<ConsumerCancelled>());
}

TEST_CASE(concurrent_transfer_moves_every_accepted_byte_once)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto consumer_sink = adopt_ref(*new LatestTicketSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto producer_sink = adopt_ref(*new CountingProducerSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *producer_sink);
    channel->attach_consumer(*consumer_sink);
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> timed_out { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> transferred { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> accepted { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> accepted_after_rejection { 0 };
    producer_sink->credit.store(channel->initial_credit(), AK::MemoryOrder::memory_order_release);

    // A transfer tells the transport nothing, so the writer learns of it from the test and from its rejected writes.
    auto writer = Threading::Thread::construct("channel writer"sv, [&channel, &producer_sink, &timed_out, &transferred, &accepted, &accepted_after_rejection]() -> intptr_t {
        for (;;) {
            auto may_write = spin_until_or_timeout([&] {
                return producer_sink->credit.load(AK::MemoryOrder::memory_order_acquire) > 0 || transferred.load();
            });
            if (!may_write) {
                timed_out.store(true);
                return 1;
            }
            if (!transferred.load())
                producer_sink->credit.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel);
            if (channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes()))) == WriteResult::Rejected)
                break;
            accepted.fetch_add(1);
        }
        for (int i = 0; i < 16; ++i) {
            if (channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes()))) == WriteResult::Accepted)
                accepted_after_rejection.fetch_add(1);
        }
        return 0;
    });
    writer->start();

    // The consumer drains a while, then transfers the body while the writer is mid-stream.
    u64 total_bytes_taken = 0;
    while (total_bytes_taken < 5'000) {
        auto ticket = channel->arm_waiter();
        auto woken = spin_until_or_timeout([&] {
            return consumer_sink->latest.load(AK::MemoryOrder::memory_order_acquire) >= ticket.value();
        });
        if (!woken) {
            timed_out.store(true);
            warnln("No wake arrived for ticket {}", ticket.value());
            break;
        }
        for (auto const& taken : channel->take_all().taken)
            total_bytes_taken += taken.size();
    }
    u64 total_bytes_moved = 0;
    for (auto const& moved : channel->transfer_out())
        total_bytes_moved += moved.size();
    transferred.store(true);

    MUST(writer->join());
    EXPECT(!timed_out.load());
    EXPECT_EQ(total_bytes_taken + total_bytes_moved, accepted.load());
    EXPECT_EQ(accepted_after_rejection.load(), 0u);
    EXPECT_EQ(producer_sink->cancellations.load(), 0u);
    EXPECT(channel->take_all().state.has<Transferred>());
}

TEST_CASE(arming_after_an_ending_fires_at_once)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    auto ticket = harness.channel->arm_waiter();
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
    EXPECT_EQ(events[0].value, ticket.value());
}

TEST_CASE(stale_ticket_identifiable_after_rearm)
{
    ChannelHarness harness;

    auto first_ticket = harness.channel->arm_waiter();
    harness.channel->disarm_waiter(first_ticket);
    auto second_ticket = harness.channel->arm_waiter();
    EXPECT(first_ticket != second_ticket);

    EXPECT_EQ(harness.channel->write(bytes_of("x"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, second_ticket.value());
}

TEST_CASE(end_only_waiter_ignores_writes)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("pending"sv)), WriteResult::Accepted);

    // Buffered bytes neither fire nor consume an EndOnly waiter.
    auto ticket = harness.channel->arm_waiter(FetchByteChannel::EndOnly {});
    EXPECT(harness.take_events().is_empty());
    EXPECT_EQ(harness.channel->write(bytes_of("more"sv)), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
    EXPECT_EQ(events[0].value, ticket.value());

    // An arm_waiter() call after the channel has ended fires at once.
    auto late_ticket = harness.channel->arm_waiter(FetchByteChannel::EndOnly {});
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, late_ticket.value());
}

TEST_CASE(waiters_fire_once_each_in_the_order_they_were_added)
{
    ChannelHarness harness;
    auto first_ticket = harness.channel->arm_waiter();
    auto end_ticket = harness.channel->arm_waiter(FetchByteChannel::EndOnly {});
    auto second_ticket = harness.channel->arm_waiter();

    EXPECT_EQ(harness.channel->write(bytes_of("a"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].value, first_ticket.value());
    EXPECT_EQ(events[1].value, second_ticket.value());

    EXPECT_EQ(harness.channel->write(bytes_of("b"sv)), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, end_ticket.value());
}

TEST_CASE(threshold_waiter_waits_for_its_byte_count)
{
    ChannelHarness harness;
    auto ticket = harness.channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 5 });

    EXPECT_EQ(harness.channel->write(bytes_of("abc"sv)), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(harness.channel->write(bytes_of("de"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, ticket.value());

    // A threshold already reached fires as the waiter is added.
    auto late_ticket = harness.channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 5 });
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, late_ticket.value());
}

TEST_CASE(threshold_waiter_fires_when_the_channel_ends_short)
{
    ChannelHarness harness;
    auto ticket = harness.channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 5 });

    EXPECT_EQ(harness.channel->write(bytes_of("ab"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, ticket.value());
}

TEST_CASE(disarming_one_waiter_keeps_the_others)
{
    ChannelHarness harness;
    auto first_ticket = harness.channel->arm_waiter();
    auto second_ticket = harness.channel->arm_waiter();
    harness.channel->disarm_waiter(first_ticket);

    EXPECT_EQ(harness.channel->write(bytes_of("a"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, second_ticket.value());
}

TEST_CASE(ending_fires_every_waiter)
{
    ChannelHarness harness;
    auto threshold_ticket = harness.channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 100 });
    auto end_ticket = harness.channel->arm_waiter(FetchByteChannel::EndOnly {});

    EXPECT_EQ(harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) }), TerminateResult::Terminated);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].value, threshold_ticket.value());
    EXPECT_EQ(events[1].value, end_ticket.value());
}

TEST_CASE(peek_copies_without_taking_or_granting_credit)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("hello "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("world"sv)), WriteResult::Accepted);
    (void)harness.take_events();

    EXPECT_EQ(flatten(harness.channel->peek_up_to(8)), "hello wo"sv);
    EXPECT_EQ(flatten(harness.channel->peek_up_to(100)), "hello world"sv);
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(flatten(harness.channel->take_up_to(3)), "hel"sv);
    EXPECT_EQ(flatten(harness.channel->peek_up_to(4)), "lo w"sv);
    EXPECT_EQ(flatten(harness.channel->take_all()), "lo world"sv);
}

TEST_CASE(peek_sees_copied_bytes_in_order)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write_copy("copied "sv.bytes()), WriteResult::Accepted);
    EXPECT_EQ(flatten(harness.channel->peek_up_to(100)), "copied "sv);

    EXPECT_EQ(harness.channel->write_copy("after"sv.bytes()), WriteResult::Accepted);
    EXPECT_EQ(flatten(harness.channel->take_all()), "copied after"sv);
}

TEST_CASE(peek_reports_how_the_channel_ended)
{
    ChannelHarness closed;
    EXPECT_EQ(closed.channel->write(bytes_of("tail"sv)), WriteResult::Accepted);
    EXPECT_EQ(closed.channel->close(), TerminateResult::Terminated);
    auto closed_peek = closed.channel->peek_up_to(100);
    EXPECT_EQ(flatten(closed_peek), "tail"sv);
    EXPECT(closed_peek.state.has<Closed>());
    EXPECT_EQ(closed_peek.buffered_byte_count, 4u);

    ChannelHarness errored;
    EXPECT_EQ(errored.channel->write(bytes_of("lost"sv)), WriteResult::Accepted);
    EXPECT_EQ(errored.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) }), TerminateResult::Terminated);
    auto errored_peek = errored.channel->peek_up_to(100);
    EXPECT(errored_peek.taken.is_empty());
    EXPECT_EQ(StringView { errored_peek.state.get<Errored>().message }, "failed"sv);
}

TEST_CASE(whole_body_is_kept_through_close_and_reads)
{
    ChannelHarness harness;
    auto whole_body = bytes_of("whole body"sv);
    harness.channel->keep_whole_body(whole_body);
    harness.channel->keep_whole_body(bytes_of("later"sv));
    EXPECT_EQ(harness.channel->write(bytes_of("whole"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    EXPECT_EQ(flatten(harness.channel->take_all()), "whole"sv);

    auto kept = harness.channel->whole_body();
    EXPECT(kept.has_value());
    EXPECT_EQ(kept->bytes().data(), whole_body.bytes().data());
    EXPECT(harness.channel->whole_body().has_value());
}

TEST_CASE(whole_body_is_dropped_when_the_body_errors_is_cancelled_or_is_transferred)
{
    ChannelHarness errored;
    errored.channel->keep_whole_body(bytes_of("whole body"sv));
    EXPECT_EQ(errored.channel->error({}), TerminateResult::Terminated);
    EXPECT(!errored.channel->whole_body().has_value());

    ChannelHarness cancelled;
    cancelled.channel->keep_whole_body(bytes_of("whole body"sv));
    EXPECT_EQ(cancelled.channel->cancel(), TerminateResult::Terminated);
    EXPECT(!cancelled.channel->whole_body().has_value());

    ChannelHarness transferred;
    transferred.channel->keep_whole_body(bytes_of("whole body"sv));
    (void)transferred.channel->transfer_out();
    EXPECT(!transferred.channel->whole_body().has_value());
}

TEST_CASE(whole_body_handed_over_after_the_body_ends_is_ignored)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
    harness.channel->keep_whole_body(bytes_of("whole body"sv));
    EXPECT(!harness.channel->whole_body().has_value());
}
