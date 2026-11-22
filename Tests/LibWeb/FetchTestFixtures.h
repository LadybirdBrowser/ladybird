/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FetchTestDocument.h"
#include <AK/RefCounted.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibIPC/Transport.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace FetchTest {

// What the algorithms of a fetch saw, which the test reads once they have run.
struct AlgorithmCalls : RefCounted<AlgorithmCalls> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<StringView> calls;
    Vector<GC::Root<Web::Fetch::Infrastructure::ResponseView>> responses;
    Optional<ByteBuffer> consumed_bytes;
    size_t call_count { 0 };
    bool consumed_body { false };
};

// What reached the steps a test set up, which the test reads once they have run.
struct ReachedSteps : RefCounted<ReachedSteps> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<ByteString> steps;
    bool marker_reached { false };
};

// The commands posted to an engine that applies none of them, for the test to hand on.
struct PostedCommands : RefCounted<PostedCommands> {
    Vector<Web::Fetch::Engine::EngineCommand> commands;
};

inline ByteString describe_transport_control(Web::Fetch::Engine::Command::TransportControl::Operation const& operation, Web::Fetch::Engine::RequestId request_id, Web::Fetch::Engine::TransportEpoch transport_epoch)
{
    using Web::Fetch::Engine::Command::TransportControl;
    auto description = operation.visit(
        [](TransportControl::StopTransport) { return ByteString { "stop transport"sv }; },
        [](TransportControl::ReleaseNavigationHold) { return ByteString { "release navigation hold"sv }; },
        [](TransportControl::ReleaseNavigationHoldUpTo const& release) { return ByteString::formatted("release navigation hold up to {}", release.byte_count); });
    return ByteString::formatted("{} of request {} of epoch {}", description, request_id.value(), transport_epoch.value());
}

// Records each command that reaches the fetch engine, and applies none of them.
class CommandRecordingEngine final : public Web::Fetch::Engine::EngineCommandSink {
public:
    explicit CommandRecordingEngine(NonnullRefPtr<ReachedSteps> reached, RefPtr<PostedCommands> posted_commands = {})
        : m_reached(move(reached))
        , m_posted_commands(move(posted_commands))
    {
    }

    virtual void handle_command(Web::Fetch::Engine::EngineCommand command) override
    {
        using namespace Web::Fetch::Engine::Command;
        command.visit(
            [&](Abort const& abort) { m_reached->steps.append(ByteString::formatted("abort {}", abort.fetch_id.value())); },
            [&](Terminate const& terminate) { m_reached->steps.append(ByteString::formatted("terminate {}", terminate.fetch_id.value())); },
            [&](Stop const& stop) { m_reached->steps.append(ByteString::formatted("stop {}", stop.fetch_id.value())); },
            [&](NavigationBodyAdopted const&) { m_reached->steps.append("navigation body adopted"); },
            [&](TransportControl const& control) { m_reached->steps.append(describe_transport_control(control.operation, control.request_id, control.transport_epoch)); },
            [](auto const&) { FAIL("unexpected command"); });
        if (m_posted_commands)
            m_posted_commands->commands.append(move(command));
    }

private:
    NonnullRefPtr<ReachedSteps> m_reached;
    RefPtr<PostedCommands> m_posted_commands;
};

struct FetchHarness {
    FetchHarness()
        : document(create_test_document())
        , realm(document->relevant_settings_object().realm())
    {
        // Each case has an event loop of its own, which the window agent's fetches have to run on.
        auto& agent = Web::HTML::surrounding_agent();
        agent.fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor(agent.network_activity_observers);
    }

    // An unknown scheme: scheme fetch returns a network error, which has no body.
    NonnullRefPtr<Web::Fetch::Infrastructure::Request> create_request() const
    {
        return create_request_for("unknown-scheme:network-error"sv);
    }

    NonnullRefPtr<Web::Fetch::Infrastructure::Request> create_request_for(StringView url_string) const
    {
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        auto request = Web::Fetch::Infrastructure::Request::create();
        request->set_url_list({ url.release_value() });
        request->set_client(document->relevant_settings_object().address());
        return request;
    }

    // Fetches from the harness's document, or from the given one.
    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch(Web::Fetch::Infrastructure::Request& request, Web::Fetch::Infrastructure::FetchAlgorithms::Input input, GC::Ptr<Web::DOM::Document> from_document = {}) const
    {
        return fetch_with(request, Web::Fetch::Infrastructure::FetchAlgorithms::create(move(input)), from_document);
    }

    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch_with(Web::Fetch::Infrastructure::Request& request, Web::Fetch::Infrastructure::FetchAlgorithms const& algorithms, GC::Ptr<Web::DOM::Document> from_document = {}) const
    {
        auto& settings = (from_document ? *from_document : *document).relevant_settings_object();
        request.set_client(settings.address());
        Web::HTML::TemporaryExecutionContext execution_context { settings.realm(), Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        return Web::Fetch::Fetching::fetch(settings.realm(), request, algorithms);
    }

    // Runs the fetch's steps and the tasks they queue until the condition holds, or for ten seconds.
    void run_tasks_until(GC::Ref<GC::Function<bool()>> condition) const
    {
        auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [] { });
        deadline->start();
        Web::HTML::main_thread_event_loop().spin_until(GC::create_function(realm->heap(), GC::captures(condition, deadline->make_weak_ptr<Core::Timer>()), [](auto& condition, auto& deadline) {
            return condition->function()() || !deadline || !deadline->is_active();
        }));
    }

    GC::Ref<Web::DOM::Document> document;
    GC::Ref<JS::Realm> realm;
};

// Runs the deferred invocations queued so far, then those that they queue.
inline void run_queued_invocations(FetchHarness const& harness)
{
    auto reached = make_ref_counted<ReachedSteps>();
    Core::deferred_invoke([reached] {
        Core::deferred_invoke([reached] { reached->marker_reached = true; });
    });
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(reached), [](auto& reached) { return reached->marker_reached; }));
}

// What a RequestServer the test stands in for has been asked to start.
struct StartedRequests : RefCounted<StartedRequests> {
    GC_HOLDS_NO_CELL_EDGES;

    IPC::Transport* request_server { nullptr };
    Vector<u64> ids;
    Vector<ByteString> methods;
};

// A RequestServer client of the agent's resource loader, whose RequestServer end of the connection the test holds and
// answers for.
class StandInRequestServer {
public:
    explicit StandInRequestServer(FetchHarness const& harness)
        : m_harness(harness)
    {
        Web::ResourceLoader::initialize(harness.realm->heap());
        auto pair = MUST(IPC::Transport::create_paired());
        m_client = make_ref_counted<Requests::RequestClient>(move(pair.local), 1);
        Web::ResourceLoader::the().set_client({}, *m_client);
        m_request_server = MUST(pair.remote_handle.create_transport());
        m_started->request_server = m_request_server.ptr();
    }

    ~StandInRequestServer()
    {
        // The client can hear of its RequestServer's death after the resource loader is gone.
        m_client->on_request_server_died = nullptr;
        Web::ResourceLoader::shut_down();
    }

    // Runs the agent's tasks until another request with the method has started, and returns its id.
    u64 wait_for_request(StringView method)
    {
        // The RequestServer end is not part of the event loop, so keep the loop from sleeping until it has news.
        auto wake_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { });
        wake_timer->start();

        auto expected = ByteString { method };
        m_harness.run_tasks_until(GC::create_function(m_harness.realm->heap(), GC::captures(m_started, m_taken_count, expected), [](auto& started, auto& taken_count, auto& expected) {
            (void)started->request_server->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
                auto message = MUST(RequestServerEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
                if (message->message_id() != Messages::RequestServer::StartRequest::static_message_id())
                    return;
                auto const& start = static_cast<Messages::RequestServer::StartRequest&>(*message);
                started->ids.append(start.request_id());
                started->methods.append(start.method());
            });
            for (size_t index = taken_count; index < started->methods.size(); ++index) {
                if (started->methods[index] == expected)
                    return true;
            }
            return false;
        }));
        for (size_t index = m_taken_count; index < m_started->methods.size(); ++index) {
            if (m_started->methods[index] == method) {
                m_taken_count = index + 1;
                return m_started->ids[index];
            }
        }
        VERIFY_NOT_REACHED();
    }

    void send(IPC::MessageBuffer message)
    {
        MUST(message.transfer_message(*m_request_server));
    }

    void send_headers(u64 request_id, u32 status_code, Vector<HTTP::Header> headers = {})
    {
        send(MUST(Messages::RequestClient::HeadersBecameAvailable::static_encode(request_id, URL::about_blank(), move(headers), status_code, Optional<String> {}, Requests::CacheState::NotCached, ByteString {})));
    }

    void send_finished(u64 request_id, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error = {})
    {
        send(MUST(Messages::RequestClient::RequestFinished::static_encode(request_id, 0, timing_info, network_error)));
    }

    void send_transferred(u64 request_id)
    {
        send(MUST(Messages::RequestClient::RequestTransferred::static_encode(request_id)));
    }

private:
    FetchHarness const& m_harness;
    RefPtr<Requests::RequestClient> m_client;
    OwnPtr<IPC::Transport> m_request_server;
    NonnullRefPtr<StartedRequests> m_started { make_ref_counted<StartedRequests>() };
    // The started requests a wait has already returned or passed over.
    size_t m_taken_count { 0 };
};

inline Web::Fetch::Infrastructure::FetchAlgorithms::Input recording_responses(GC::Heap& heap, NonnullRefPtr<AlgorithmCalls> const& record)
{
    Web::Fetch::Infrastructure::FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<Web::Fetch::Infrastructure::ResponseView> response) {
        record->responses.append(GC::make_root(response));
    });
    return input;
}

inline void run_until_responses(FetchHarness const& harness, NonnullRefPtr<AlgorithmCalls> const& record, size_t count)
{
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(record, count), [](auto& record, auto& count) { return record->responses.size() == count; }));
    VERIFY(record->responses.size() == count);
}

}
