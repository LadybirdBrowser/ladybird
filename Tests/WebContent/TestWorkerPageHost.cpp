/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <WebContent/WebWorkerServerEndpoint.h>
#include <WebContent/Worker/ConnectionFromClient.h>
#include <WebContent/Worker/DedicatedWorkerThreadLink.h>
#include <WebContent/Worker/PageHost.h>

TEST_SETUP
{
    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);
}

// Runs the callback on a thread that hosts a worker's agent. The other end of the worker's connection stands in for
// the Hub.
template<typename Callback>
static void run_on_worker_thread(Callback const& callback)
{
    auto thread = Threading::Thread::construct("WorkerAgent"sv, [&callback] -> intptr_t {
        IPC::ScopedSharedIOThread shared_io_thread;
        Core::EventLoop event_loop;
        Web::Bindings::initialize_agent(Web::HTML::AgentType::SharedWorker);
        auto& agent = Web::HTML::surrounding_agent();
        agent.fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor(agent.network_activity_observers);

        auto pair = MUST(IPC::Transport::create_paired());
        auto client = WebWorker::ConnectionFromClient::construct(move(pair.local), nullptr);
        callback(*client, move(pair.remote_handle));

        event_loop.exec();
        client->shut_down_agent();
        return 0;
    });
    thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);
    thread->start();
    MUST(thread->join());
}

TEST_CASE(every_host_is_a_known_hsts_host_once_the_hub_is_gone)
{
    run_on_worker_thread([](WebWorker::ConnectionFromClient& client, IPC::TransportHandle hub) {
        {
            auto hung_up_hub = move(hub);
        }
        EXPECT(client.page_host().page_did_is_known_hsts_host("example.test"_string));
    });
}

TEST_CASE(every_host_is_a_known_hsts_host_once_the_hub_closes_the_worker)
{
    run_on_worker_thread([](WebWorker::ConnectionFromClient& client, IPC::TransportHandle hub) {
        auto hub_transport = MUST(hub.create_transport());
        auto close_worker = MUST(Messages::WebWorkerServer::CloseWorker::static_encode());
        MUST(close_worker.transfer_message(*hub_transport));
        EXPECT(client.page_host().page_did_is_known_hsts_host("example.test"_string));
    });
}
