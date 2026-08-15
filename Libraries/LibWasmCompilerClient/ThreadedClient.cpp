/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibThreading/Thread.h>
#include <LibWasmCompilerClient/Client.h>
#include <LibWasmCompilerClient/ThreadedClient.h>

namespace WasmCompilerClient {

ErrorOr<NonnullRefPtr<ThreadedClient>> ThreadedClient::create(NonnullOwnPtr<IPC::Transport> transport)
{
    auto client = adopt_ref(*new ThreadedClient(move(transport)));

    Optional<Error> initialization_error;
    {
        Sync::MutexLocker locker(client->m_mutex);
        client->m_initialization_condition.wait_while([&]() { return !client->m_initialized; });
        initialization_error = move(client->m_initialization_error);
    }

    if (initialization_error.has_value())
        return initialization_error.release_value();

    return client;
}

ThreadedClient::ThreadedClient(NonnullOwnPtr<IPC::Transport> transport)
    : m_thread(Threading::Thread::construct("WasmCompiler IPC"sv, [this, transport = move(transport)]() mutable {
        return thread_main(move(transport));
    }))
{
    m_thread->start();
}

ThreadedClient::~ThreadedClient()
{
    RefPtr<Core::WeakEventLoopReference> event_loop;
    {
        Sync::MutexLocker locker(m_mutex);
        event_loop = m_event_loop;
    }

    if (event_loop) {
        if (auto strong_event_loop = event_loop->take()) {
            strong_event_loop->deferred_invoke([]() {
                Core::EventLoop::current().quit(0);
            });
        }
    }

    if (m_thread->needs_to_be_joined())
        (void)m_thread->join();
}

Core::AnonymousBuffer ThreadedClient::compile(Core::AnonymousBuffer const& buffer)
{
    Client* client = nullptr;
    {
        Sync::MutexLocker locker(m_mutex);
        client = m_client;
        if (!client)
            return {};
        ++m_active_compilations;
    }

    ScopeGuard compilation_finished = [&]() {
        Sync::MutexLocker locker(m_mutex);
        VERIFY(m_active_compilations > 0);

        if (--m_active_compilations == 0)
            m_client_unused_condition.broadcast();
    };

    return client->compile(buffer);
}

intptr_t ThreadedClient::thread_main(NonnullOwnPtr<IPC::Transport> transport)
{
    Core::EventLoop event_loop;
    auto client_or_error = try_make_ref_counted<Client>(move(transport));

    if (client_or_error.is_error()) {
        Sync::MutexLocker locker(m_mutex);
        m_initialization_error = client_or_error.release_error();
        m_initialized = true;
        m_initialization_condition.broadcast();
        return 1;
    }

    auto client = client_or_error.release_value();

#ifdef AK_OS_WINDOWS
    auto response = client->send_sync_but_allow_failure<Client::InitTransport>(Core::System::getpid());
    if (!response) {
        Sync::MutexLocker locker(m_mutex);
        m_initialization_error = Error::from_string_literal("Failed to initialize WebAssembly compiler transport");
        m_initialized = true;
        m_initialization_condition.broadcast();
        return 1;
    }
    client->transport().set_peer_pid(response->peer_pid());
#endif

    {
        Sync::MutexLocker locker(m_mutex);

        m_event_loop = Core::EventLoop::current_weak();
        m_client = client.ptr();
        m_initialized = true;
        m_initialization_condition.broadcast();
    }

    auto result = event_loop.exec();

    {
        Sync::MutexLocker locker(m_mutex);
        m_client = nullptr;
    }

    if (client->is_open())
        client->shutdown();

    {
        Sync::MutexLocker locker(m_mutex);
        m_client_unused_condition.wait_while([&]() { return m_active_compilations != 0; });
        m_event_loop.clear();
    }

    return result;
}

}
