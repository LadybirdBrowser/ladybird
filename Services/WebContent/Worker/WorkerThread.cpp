/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/Runtime/VM.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <WebContent/Worker/ConnectionFromClient.h>
#include <WebContent/Worker/DedicatedWorkerThreadLink.h>
#include <WebContent/Worker/WorkerThread.h>

namespace WebWorker {

struct WorkerThreads {
    Mutex mutex;
    Vector<NonnullRefPtr<Core::WeakEventLoopReference>> event_loops;
};

static WorkerThreads& worker_threads()
{
    static NeverDestroyed<WorkerThreads> threads;
    return *threads;
}

static thread_local bool s_is_worker_thread { false };

static Atomic<size_t, AK::MemoryOrder::memory_order_relaxed> s_worker_thread_count { 0 };

bool is_worker_thread()
{
    return s_is_worker_thread;
}

static intptr_t run_worker_thread(IPC::TransportHandle const& worker_handle, Web::HTML::AgentType agent_type, RefPtr<DedicatedWorkerThreadLink> const& dedicated_worker_thread_link)
{
    s_is_worker_thread = true;

    // NB: A worker has several connections, none of them busy for long, and most workers do not use most of them.
    IPC::ScopedSharedIOThread shared_io_thread;

    auto transport_or_error = worker_handle.create_transport();
    if (transport_or_error.is_error()) {
        dbgln("Unable to take up the transport of a worker: {}", transport_or_error.error());
        --s_worker_thread_count;
        if (dedicated_worker_thread_link)
            dedicated_worker_thread_link->report_death_to_owner();
        return 1;
    }

    Core::EventLoop event_loop;
    Web::Bindings::initialize_agent(agent_type);
    Web::HTML::surrounding_agent().fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor();

    auto event_loop_reference = Core::EventLoop::current_weak();
    {
        MutexLocker locker(worker_threads().mutex);
        worker_threads().event_loops.append(event_loop_reference);
    }

    auto client = ConnectionFromClient::construct(transport_or_error.release_value(), dedicated_worker_thread_link);
    client->set_up_agent_services();
    if (dedicated_worker_thread_link)
        dedicated_worker_thread_link->attach_worker_connection(*client);

    auto exit_code = event_loop.exec();

    if (dedicated_worker_thread_link)
        dedicated_worker_thread_link->detach_worker_connection();

    {
        MutexLocker locker(worker_threads().mutex);
        worker_threads().event_loops.remove_first_matching([&](auto const& reference) { return reference.ptr() == event_loop_reference.ptr(); });
    }

    --s_worker_thread_count;
    client->shut_down_agent();
    return exit_code;
}

static ErrorOr<void> start_thread(IPC::TransportHandle worker_handle, Web::HTML::AgentType agent_type, RefPtr<DedicatedWorkerThreadLink> dedicated_worker_thread_link)
{
    auto thread = TRY(Threading::Thread::try_create("WebWorker"sv, [worker_handle = move(worker_handle), agent_type, dedicated_worker_thread_link = move(dedicated_worker_thread_link)] {
        return run_worker_thread(worker_handle, agent_type, dedicated_worker_thread_link);
    }));
    thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);

    ++s_worker_thread_count;
    if (auto result = thread->try_start(); result.is_error()) {
        --s_worker_thread_count;
        return result.release_error();
    }

    // NB: Nothing waits for a worker's thread: one that is stuck in a long builtin must not hold up anything else.
    thread->detach();
    return {};
}

ErrorOr<void> start_worker_thread(IPC::TransportHandle worker_handle, Web::HTML::AgentType agent_type)
{
    return start_thread(move(worker_handle), agent_type, nullptr);
}

ErrorOr<StartedDedicatedWorkerThread> start_dedicated_worker_thread(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    if (s_worker_thread_count >= Web::HTML::maximum_worker_count)
        return Error::from_string_literal("This process already runs as many workers as it may");

    auto worker_connection = TRY(IPC::TransportHandle::create_pair());
    auto link = DedicatedWorkerThreadLink::create(owner_token);
    TRY(start_thread(move(worker_connection.first), Web::HTML::AgentType::DedicatedWorker, link));
    return StartedDedicatedWorkerThread { move(link), move(worker_connection.second) };
}

void post_to_worker_threads(void (*function)())
{
    MutexLocker locker(worker_threads().mutex);
    for (auto& reference : worker_threads().event_loops) {
        if (auto event_loop = reference->take(); event_loop.is_alive())
            event_loop->deferred_invoke([function] { function(); });
    }
}

}
