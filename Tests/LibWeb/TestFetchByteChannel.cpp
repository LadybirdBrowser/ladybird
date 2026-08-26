/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
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
        , channel(FetchByteChannel::create(watermarks, *consumer_sink, *producer_sink))
    {
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

ByteString flatten(FetchByteChannel::TakeResult const& result)
{
    ByteBuffer flat;
    for (auto const& taken : result.taken)
        flat.append(taken.bytes());
    return ByteString { flat.bytes() };
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
    auto consumer_sink = adopt_ref(*new RecordingConsumerSink(events));
    auto producer_sink = adopt_ref(*new RecordingProducerSink(events));
    EXPECT_DEATH("a channel without a low watermark never refills its credit", (void)FetchByteChannel::create({ .low = 0, .high = 64 }, *consumer_sink, *producer_sink));
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

TEST_CASE(arming_the_waiter_twice_dies)
{
    // The death leaves the channel locked.
    static auto& harness = *new ChannelHarness;
    (void)harness.channel->arm_waiter();
    EXPECT_DEATH("a waiter is armed once per pull", (void)harness.channel->arm_waiter());
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

TEST_CASE(waiter_is_one_shot)
{
    ChannelHarness harness;

    (void)harness.channel->arm_waiter();
    EXPECT_EQ(harness.channel->write(bytes_of("a"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("b"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.take_events().size(), 1u);
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
    };
    for (auto ending : { Ending::Close, Ending::Error, Ending::Cancel }) {
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
    IGNORE_USE_IN_ESCAPING_LAMBDA auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *consumer_sink, *producer_sink);
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

TEST_CASE(concurrent_cancellation_rejects_every_later_write)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto consumer_sink = adopt_ref(*new LatestTicketSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto producer_sink = adopt_ref(*new CountingProducerSink);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *consumer_sink, *producer_sink);
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
