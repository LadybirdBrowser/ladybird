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
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>

using namespace Web::Fetch::Engine;

namespace {

// Records what reaches the engine, and on which thread.
struct RecordedCommands {
    Vector<u64> fetch_ids;
    Vector<AK::ThreadID> threads;
    Optional<IPC::TransportHandle> adopted_transport;
    FetchExecutor* attached_executor { nullptr };
    // The engine's records, which defer work through the executor that runs the engine.
    FetchEngineRecords* records { nullptr };
};

class RecordingSink final : public EngineCommandSink {
public:
    explicit RecordingSink(RecordedCommands& recorded)
        : m_recorded(recorded)
    {
        m_recorded.records = &m_records;
    }

    virtual void attach_to_executor(FetchExecutor& executor) override
    {
        m_recorded.attached_executor = &executor;
        m_records.attach_to_executor(executor);
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
    FetchEngineRecords m_records { make<FetchEngineRecords::Agent>() };
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

TEST_CASE(commands_run_in_posting_order_when_a_command_spins_a_nested_loop)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    RefPtr<InlineFetchExecutor> executor;

    // The first command posts a third one and spins a nested loop until it has run, as a sync XHR does.
    class SpinningSink final : public EngineCommandSink {
    public:
        SpinningSink(RecordedCommands& recorded, RefPtr<InlineFetchExecutor>& executor)
            : m_recorded(recorded)
            , m_executor(executor)
        {
        }

        virtual void attach_to_executor(FetchExecutor&) override { }

        virtual void handle_command(EngineCommand command) override
        {
            auto fetch_id = command.get<Command::Stop>().fetch_id.value();
            m_recorded.fetch_ids.append(fetch_id);
            if (fetch_id != 1)
                return;
            m_executor->post(Command::Stop { .fetch_id = FetchId(3) });
            // NB: The cap keeps the test finite when the nested loop does not run the third command.
            for (auto turns = 0; turns < 10 && !m_recorded.fetch_ids.contains_slow(3); ++turns)
                Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
            m_ran_inside_nested_loop = m_recorded.fetch_ids.contains_slow(3);
        }

        bool ran_inside_nested_loop() const { return m_ran_inside_nested_loop; }

    private:
        RecordedCommands& m_recorded;
        RefPtr<InlineFetchExecutor>& m_executor;
        bool m_ran_inside_nested_loop { false };
    };

    auto sink = make<SpinningSink>(recorded, executor);
    auto& spinning_sink = *sink;
    executor = InlineFetchExecutor::create(services_for_current_thread(), move(sink));
    executor->post(Command::Stop { .fetch_id = FetchId(1) });
    executor->post(Command::Stop { .fetch_id = FetchId(2) });

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(recorded.fetch_ids, (Vector<u64> { 1, 2, 3 }));
    EXPECT(spinning_sink.ran_inside_nested_loop());
    EXPECT(!has_queued_tasks(*executor));
    executor = nullptr;
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

TEST_CASE(an_engine_is_attached_to_the_executor_that_runs_it)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
    EXPECT_EQ(recorded.attached_executor, executor.ptr());
}

TEST_CASE(deferred_work_runs_on_the_loop_after_the_commands_posted_before_it)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));

    executor->post(Command::Stop { .fetch_id = FetchId(1) });
    recorded.records->defer([&] { recorded.fetch_ids.append(2); });
    executor->post(Command::Stop { .fetch_id = FetchId(3) });
    EXPECT(recorded.fetch_ids.is_empty());

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(recorded.fetch_ids, (Vector<u64> { 1, 2, 3 }));
}

TEST_CASE(deferred_work_is_dropped_by_shutdown)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
    size_t runs = 0;

    recorded.records->defer([&] { ++runs; });
    executor->shutdown();
    recorded.records->defer([&] { ++runs; });

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(runs, 0u);
    EXPECT(!has_queued_tasks(*executor));
}
