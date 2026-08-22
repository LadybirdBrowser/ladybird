/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Thread.h>
#include <LibWasmCompilerClient/Client.h>
#include <LibWasmCompilerClient/ThreadedClient.h>

namespace WasmCompilerClient {

ErrorOr<NonnullRefPtr<ThreadedClient>> ThreadedClient::create(IPC::TransportHandle handle)
{
    auto client = adopt_ref(*new ThreadedClient(move(handle)));

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

ThreadedClient::ThreadedClient(IPC::TransportHandle handle)
    : m_thread(Threading::Thread::construct("WasmCompiler IPC"sv, [this, handle = move(handle)]() {
        return thread_main(handle);
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

// This must run on the IPC thread. On Windows, the socket's notifier is bound to the event loop of the thread that
// creates the socket, and the connection must be serviced by that same thread.
static ErrorOr<NonnullRefPtr<Client>> create_client(IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    auto client = TRY(try_make_ref_counted<Client>(move(transport)));

#ifdef AK_OS_WINDOWS
    if (auto response = client->send_sync_but_allow_failure<Client::InitTransport>(Core::System::getpid()))
        client->transport().set_peer_pid(response->peer_pid());
    else
        return Error::from_string_literal("Failed to initialize WebAssembly compiler transport");
#endif

    return client;
}

intptr_t ThreadedClient::thread_main(IPC::TransportHandle const& handle)
{
    Core::EventLoop event_loop;
    auto client_or_error = create_client(handle);

    if (client_or_error.is_error()) {
        Sync::MutexLocker locker(m_mutex);
        m_initialization_error = client_or_error.release_error();
        m_initialized = true;
        m_initialization_condition.broadcast();
        return 1;
    }

    auto client = client_or_error.release_value();

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
