/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>

namespace {

using Web::Fetch::Engine::FetchBodyDeliveryGate;
using Web::Fetch::Engine::WholeBodyTransport;
using DeliveryResult = FetchBodyDeliveryGate::DeliveryResult;
using NavigationHold = FetchBodyDeliveryGate::NavigationHold;

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

struct TransportCall {
    enum class Op : u8 {
        Resume,
        Pause,
        Stop,
    };
    Op op;
    u64 value { 0 };

    bool operator==(TransportCall const&) const = default;
};

struct RecordingTransport final : FetchBodyDeliveryGate::Transport {
    virtual void resume_body_delivery_up_to(u64 byte_count) override { calls.append({ TransportCall::Op::Resume, byte_count }); }
    virtual void pause_body_delivery() override { calls.append({ TransportCall::Op::Pause, 0 }); }
    virtual void stop() override { calls.append({ TransportCall::Op::Stop, 0 }); }

    virtual void misbehaved(StringView reason) override
    {
        if (!records_misbehavior) {
            FetchBodyDeliveryGate::Transport::misbehaved(reason);
            return;
        }
        misbehaviors.append(ByteString { reason });
    }

    Vector<TransportCall> take_calls() { return move(calls); }

    Vector<TransportCall> calls;
    bool records_misbehavior { false };
    Vector<ByteString> misbehaviors;
};

struct GateHarness {
    explicit GateHarness(NavigationHold hold)
        : gate(FetchBodyDeliveryGate::create(hold))
        , transport(adopt_ref(*new RecordingTransport))
    {
    }

    ~GateHarness() { gate->detach_transport(); }

    NonnullRefPtr<FetchBodyDeliveryGate> gate;
    NonnullRefPtr<RecordingTransport> transport;
};

}

TEST_CASE(a_delivery_beyond_the_allowance_dies)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(10);
    EXPECT_EQ(harness.gate->did_deliver(10), DeliveryResult::Accepted);
    EXPECT_DEATH("the transport delivered more than it was allowed", (void)harness.gate->did_deliver(1));
}

TEST_CASE(a_rejected_delivery_leaves_the_allowance_unspent)
{
    GateHarness harness { NavigationHold::None };
    harness.transport->records_misbehavior = true;
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(10);

    EXPECT_EQ(harness.gate->did_deliver(4), DeliveryResult::Accepted);
    EXPECT_EQ(harness.gate->did_deliver(7), DeliveryResult::Rejected);
    EXPECT_EQ(harness.transport->misbehaviors, (Vector<ByteString> { "The transport delivered more than it was allowed" }));
    EXPECT_EQ(harness.gate->did_deliver(6), DeliveryResult::Accepted);
}

TEST_CASE(destroying_an_attached_gate_dies)
{
    EXPECT_DEATH("a gate is detached before it is destroyed", [] {
        auto gate = FetchBodyDeliveryGate::create(NavigationHold::None);
        gate->attach_transport(adopt_ref(*new RecordingTransport));
    }());
}

TEST_CASE(navigation_hold_blocks_credited_delivery)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Pause } }));

    // Channel demand alone must never release the navigation hold.
    harness.gate->grant_credit(100);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Pause } }));
}

TEST_CASE(bounded_hold_release_permits_bytes_from_the_current_delivery_point)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(100);
    (void)harness.transport->take_calls();

    harness.gate->release_hold_up_to(10);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 10 } }));

    // A further bounded release counts from what has been delivered since.
    EXPECT_EQ(harness.gate->did_deliver(10), DeliveryResult::Accepted);
    harness.gate->release_hold_up_to(5);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 5 } }));
}

TEST_CASE(hold_release_cannot_bypass_channel_backpressure)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(10);
    (void)harness.transport->take_calls();

    // The full hold release leaves the channel's credit as the only bound.
    harness.gate->release_hold();
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 10 } }));

    EXPECT_EQ(harness.gate->did_deliver(10), DeliveryResult::Accepted);
    harness.gate->release_hold();
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Pause } }));

    harness.gate->grant_credit(30);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 30 } }));
}

TEST_CASE(delivery_consumes_the_smaller_of_both_allowances)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(100);
    harness.gate->release_hold_up_to(20);
    (void)harness.transport->take_calls();

    EXPECT_EQ(harness.gate->did_deliver(15), DeliveryResult::Accepted);
    harness.gate->grant_credit(50);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 5 } }));
}

TEST_CASE(allowances_granted_before_attach_apply_at_attach)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->grant_credit(40);
    harness.gate->attach_transport(harness.transport);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 40 } }));
}

TEST_CASE(attach_after_termination_stops_the_transport)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->consumer_cancelled();
    harness.gate->attach_transport(harness.transport);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Stop } }));
}

TEST_CASE(consumer_cancellation_stops_an_attached_transport_once)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(10);
    (void)harness.transport->take_calls();

    harness.gate->consumer_cancelled();
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Stop } }));

    harness.gate->consumer_cancelled();
    harness.gate->grant_credit(10);
    harness.gate->release_hold();
    EXPECT(harness.transport->take_calls().is_empty());
}

TEST_CASE(detached_transport_is_never_touched_again)
{
    // Teardown detaches the gate before the channel cancellation reaches it.
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    (void)harness.transport->take_calls();

    harness.gate->detach_transport();
    harness.gate->consumer_cancelled();
    harness.gate->grant_credit(10);
    harness.gate->release_hold();
    EXPECT(harness.transport->take_calls().is_empty());
}

TEST_CASE(channel_sink_posts_commands_onto_the_creating_loop)
{
    Core::EventLoop loop;
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    (void)harness.transport->take_calls();

    auto sink = harness.gate->create_channel_sink();
    sink->grant_credit(25);
    EXPECT(harness.transport->take_calls().is_empty());

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 25 } }));

    sink->consumer_cancelled();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Stop } }));
}

// A command posted after the gate's loop is gone is dropped rather than run on the poster's thread.
TEST_CASE(channel_sink_drops_commands_once_the_creating_loop_is_gone)
{
    Core::EventLoop main_loop;
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Web::Fetch::Engine::FetchByteChannelProducerSink> sink;
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<RecordingTransport> transport;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> loop_gone { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> commands_posted { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> timed_out { false };

    // The gate outlives its loop, on its own thread, until the commands have been posted.
    auto owner = Threading::Thread::construct("gate owner"sv, [&]() -> intptr_t {
        auto gate = FetchBodyDeliveryGate::create(NavigationHold::None);
        {
            Core::EventLoop loop;
            transport = adopt_ref(*new RecordingTransport);
            gate->attach_transport(*transport);
            (void)transport->take_calls();
            sink = gate->create_channel_sink();
        }
        loop_gone.store(true);
        if (!spin_until_or_timeout([&] { return commands_posted.load(); }))
            timed_out.store(true);
        gate->detach_transport();
        return 0;
    });
    owner->start();

    if (spin_until_or_timeout([&] { return loop_gone.load(); })) {
        sink->grant_credit(25);
        sink->consumer_cancelled();
    } else {
        timed_out.store(true);
    }
    commands_posted.store(true);
    MUST(owner->join());

    main_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!timed_out.load());
    EXPECT(transport->take_calls().is_empty());
}

namespace {

struct WholeBodyHarness {
    explicit WholeBodyHarness(NavigationHold hold)
        : gate(FetchBodyDeliveryGate::create(hold))
        , transport(WholeBodyTransport::create())
    {
        transport->attach_to(gate);
    }

    ~WholeBodyHarness() { gate->detach_transport(); }

    void set_body(size_t size)
    {
        auto body = MUST(ByteBuffer::create_zeroed(size));
        transport->set_body(Core::ImmutableBytes::adopt(move(body)), [this](Requests::ResponseData data) {
            EXPECT_EQ(gate->did_deliver(data.bytes().size()), DeliveryResult::Accepted);
            deliveries.append(data.bytes().size());
        });
    }

    Vector<size_t> take_deliveries() { return move(deliveries); }

    Core::EventLoop loop;
    NonnullRefPtr<FetchBodyDeliveryGate> gate;
    NonnullRefPtr<WholeBodyTransport> transport;
    Vector<size_t> deliveries;
};

}

TEST_CASE(whole_body_transport_delivers_within_the_allowance)
{
    WholeBodyHarness harness { NavigationHold::None };
    harness.gate->grant_credit(300);
    harness.set_body(1000);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 300 }));

    harness.gate->grant_credit(500);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 500 }));

    harness.gate->grant_credit(1000);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 200 }));
}

TEST_CASE(whole_body_transport_reports_delivery_after_the_last_piece)
{
    WholeBodyHarness harness { NavigationHold::None };
    harness.gate->grant_credit(600);
    harness.set_body(1000);

    bool delivered = false;
    harness.transport->when_delivered([&] { delivered = true; });
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!delivered);

    harness.gate->grant_credit(400);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 600, 400 }));
    EXPECT(delivered);
}

TEST_CASE(whole_body_transport_reports_delivery_only_once_it_has_a_body)
{
    WholeBodyHarness harness { NavigationHold::None };
    harness.gate->grant_credit(1000);

    bool delivered = false;
    harness.transport->when_delivered([&] { delivered = true; });
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!delivered);

    harness.set_body(100);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 100 }));
    EXPECT(delivered);
}

TEST_CASE(whole_body_transport_waits_for_the_navigation_hold)
{
    WholeBodyHarness harness { NavigationHold::Held };
    harness.gate->grant_credit(1000);
    harness.set_body(100);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(harness.take_deliveries().is_empty());

    harness.gate->release_hold();
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 100 }));
}

TEST_CASE(whole_body_transport_stops_when_the_consumer_cancels)
{
    WholeBodyHarness harness { NavigationHold::None };
    harness.gate->grant_credit(100);
    harness.set_body(1000);

    bool delivered = false;
    harness.transport->when_delivered([&] { delivered = true; });
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 100 }));

    harness.gate->consumer_cancelled();
    harness.gate->grant_credit(1000);
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(harness.take_deliveries().is_empty());
    EXPECT(!delivered);
}

namespace {

struct CallbackMarker final : public RefCounted<CallbackMarker> { };

}

TEST_CASE(stopping_a_whole_body_transport_releases_its_callbacks_and_runs_its_stop_steps_once)
{
    WholeBodyHarness harness { NavigationHold::None };
    harness.gate->grant_credit(100);
    auto marker = adopt_ref(*new CallbackMarker);
    auto body = MUST(ByteBuffer::create_zeroed(1000));
    harness.transport->set_body(Core::ImmutableBytes::adopt(move(body)), [&harness, marker](Requests::ResponseData data) {
        EXPECT_EQ(harness.gate->did_deliver(data.bytes().size()), DeliveryResult::Accepted);
    });
    harness.transport->when_delivered([marker] { });
    int stop_steps_run = 0;
    harness.transport->set_on_stop([&] { ++stop_steps_run; });
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(marker->ref_count(), 3u);

    harness.transport->stop();
    EXPECT_EQ(marker->ref_count(), 1u);
    EXPECT_EQ(stop_steps_run, 1);

    harness.transport->stop();
    EXPECT_EQ(stop_steps_run, 1);
}

TEST_CASE(a_whole_body_transport_runs_no_stop_steps_after_delivering_the_whole_body)
{
    WholeBodyHarness harness { NavigationHold::None };
    harness.gate->grant_credit(1000);
    harness.set_body(100);
    bool stop_steps_ran = false;
    harness.transport->set_on_stop([&] { stop_steps_ran = true; });
    harness.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.take_deliveries(), (Vector<size_t> { 100 }));

    harness.transport->stop();
    EXPECT(!stop_steps_ran);
}
