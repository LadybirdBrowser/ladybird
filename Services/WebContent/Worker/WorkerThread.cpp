/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
        if (dedicated_worker_thread_link)
            dedicated_worker_thread_link->report_death_to_owner();
        return 1;
    }

    Core::EventLoop event_loop;
    Web::Bindings::initialize_agent(agent_type);

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

    client->shut_down_agent();
    return exit_code;
}

static ErrorOr<void> start_thread(IPC::TransportHandle worker_handle, Web::HTML::AgentType agent_type, RefPtr<DedicatedWorkerThreadLink> dedicated_worker_thread_link)
{
    auto thread = TRY(Threading::Thread::try_create("WebWorker"sv, [worker_handle = move(worker_handle), agent_type, dedicated_worker_thread_link = move(dedicated_worker_thread_link)] {
        return run_worker_thread(worker_handle, agent_type, dedicated_worker_thread_link);
    }));
    thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);

    TRY(thread->try_start());

    // NB: Nothing waits for a worker's thread: one that is stuck in a long builtin must not hold up anything else.
    thread->detach();
    return {};
}

ErrorOr<void> start_worker_thread(IPC::TransportHandle worker_handle, Web::HTML::AgentType agent_type)
{
    return start_thread(move(worker_handle), agent_type, nullptr);
}

RefPtr<Web::HTML::DedicatedWorkerThreadLink> start_worker_thread_for_owner(IPC::TransportHandle worker_handle, Web::HTML::AgentType agent_type, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    RefPtr<DedicatedWorkerThreadLink> dedicated_worker_thread_link;
    if (agent_type == Web::HTML::AgentType::DedicatedWorker)
        dedicated_worker_thread_link = DedicatedWorkerThreadLink::create(owner_token);

    if (auto result = start_thread(move(worker_handle), agent_type, dedicated_worker_thread_link); result.is_error()) {
        dbgln("Unable to start a thread for a worker: {}", result.error());
        return nullptr;
    }
    return dedicated_worker_thread_link;
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
