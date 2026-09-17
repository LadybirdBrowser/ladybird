/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ThreadID.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>

using namespace Web::Fetch::Engine;

namespace {

// Records what reaches the engine, and on which thread.
struct RecordedCommands {
    Vector<u64> fetch_ids;
    Vector<AK::ThreadID> threads;
    Optional<IPC::TransportHandle> adopted_transport;
};

class RecordingSink final : public EngineCommandSink {
public:
    explicit RecordingSink(RecordedCommands& recorded)
        : m_recorded(recorded)
    {
    }

    virtual void handle_command(EngineCommand command) override
    {
        m_recorded.threads.append(AK::ThreadID::current());
        command.visit(
            [&](Command::Stop const& stop) { m_recorded.fetch_ids.append(stop.fetch_id.value()); },
            [&](Command::AdoptNetworkTransport& adopt) { m_recorded.adopted_transport = move(adopt.handle); },
            [](auto const&) { FAIL("unexpected command"); });
    }

private:
    RecordedCommands& m_recorded;
};

FetchExecutorServices services_for_current_thread()
{
    return { .event_loop = Core::EventLoop::current_weak() };
}

// Each task the executor posts holds it until the task has run or been dropped.
bool has_queued_tasks(InlineFetchExecutor const& executor)
{
    return executor.ref_count() > 1;
}

bool peer_has_hung_up(IPC::Transport& transport)
{
    return transport.read_as_many_messages_as_possible_without_blocking([](auto&&) { }) == IPC::Transport::ShouldShutdown::Yes;
}

void wait_for_peer_to_hang_up(IPC::Transport& transport)
{
    while (!peer_has_hung_up(transport))
        transport.wait_until_readable();
}

}

TEST_CASE(commands_run_on_the_loop_in_posting_order)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));

    for (u64 id = 1; id <= 3; ++id)
        executor->post(Command::Stop { .fetch_id = FetchId(id) });
    EXPECT(recorded.fetch_ids.is_empty());

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(recorded.fetch_ids, (Vector<u64> { 1, 2, 3 }));
    for (auto thread : recorded.threads)
        EXPECT(thread.is_current_thread());
}

TEST_CASE(an_executor_on_another_thread_runs_its_commands_there)
{
    Core::EventLoop main_loop;
    IGNORE_USE_IN_ESCAPING_LAMBDA auto main_loop_reference = Core::EventLoop::current_weak();
    IGNORE_USE_IN_ESCAPING_LAMBDA RecordedCommands recorded;
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<InlineFetchExecutor> executor;
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> executor_loop;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> release { false };

    auto thread = Threading::Thread::construct("FetchExecutor"sv, [&]() -> intptr_t {
        Core::EventLoop loop;
        auto created = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
        main_loop_reference->take()->deferred_invoke([&, created, loop_reference = Core::EventLoop::current_weak()] {
            executor = created;
            executor_loop = loop_reference;
        });
        while (!release.load())
            loop.pump(Core::EventLoop::WaitMode::WaitForEvents);
        return 0;
    });
    thread->start();

    while (!executor)
        main_loop.pump(Core::EventLoop::WaitMode::WaitForEvents);
    for (u64 id = 1; id <= 3; ++id)
        executor->post(Command::Stop { .fetch_id = FetchId(id) });
    executor->post(Command::AdoptNetworkTransport { .handle = {} });
    executor_loop->take()->deferred_invoke([&] { release.store(true); });
    MUST(thread->join());

    EXPECT_EQ(recorded.fetch_ids, (Vector<u64> { 1, 2, 3 }));
    EXPECT_EQ(recorded.threads.size(), 4u);
    for (auto recorded_thread : recorded.threads) {
        EXPECT(!recorded_thread.is_current_thread());
        EXPECT_EQ(recorded_thread, recorded.threads.first());
    }
}

TEST_CASE(a_command_posted_after_shutdown_is_destroyed_at_once)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
    auto pair = MUST(IPC::Transport::create_paired());

    executor->shutdown();
    executor->post(Command::AdoptNetworkTransport { .handle = move(pair.remote_handle) });
    EXPECT(!has_queued_tasks(*executor));
    wait_for_peer_to_hang_up(*pair.local);

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(recorded.threads.is_empty());
}

TEST_CASE(a_command_posted_after_its_loop_is_gone_is_destroyed_at_once)
{
    RecordedCommands recorded;
    RefPtr<InlineFetchExecutor> executor;
    {
        Core::EventLoop loop;
        executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
    }
    auto pair = MUST(IPC::Transport::create_paired());

    executor->post(Command::AdoptNetworkTransport { .handle = move(pair.remote_handle) });
    EXPECT(!has_queued_tasks(*executor));
    wait_for_peer_to_hang_up(*pair.local);
    EXPECT(recorded.threads.is_empty());
}

TEST_CASE(a_pending_command_is_dropped_by_shutdown)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
    auto pair = MUST(IPC::Transport::create_paired());

    executor->post(Command::AdoptNetworkTransport { .handle = move(pair.remote_handle) });
    executor->shutdown();
    EXPECT(has_queued_tasks(*executor));
    EXPECT(!peer_has_hung_up(*pair.local));

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(recorded.threads.is_empty());
    EXPECT(!has_queued_tasks(*executor));
    wait_for_peer_to_hang_up(*pair.local);
}

TEST_CASE(a_network_transport_is_adopted_exactly_once)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
    auto pair = MUST(IPC::Transport::create_paired());

    executor->post(Command::AdoptNetworkTransport { .handle = move(pair.remote_handle) });
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    // The engine owns the handle until it drops it.
    EXPECT(recorded.adopted_transport.has_value());
    EXPECT(!has_queued_tasks(*executor));
    EXPECT(!peer_has_hung_up(*pair.local));
    recorded.adopted_transport.clear();
    wait_for_peer_to_hang_up(*pair.local);
}
