/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FetchTestDocument.h"
#include <AK/RefCounted.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
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

// The responses the fetch engine has handed over to the fetches' agent, in the order it handed them over.
struct HandedOverResponses : RefCounted<HandedOverResponses> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<NonnullRefPtr<Web::Fetch::Infrastructure::Response const>> responses;
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

// Hands each event of a fetch on to the fetch's event sink, and records the ends of the fetch's bodies and of the fetch,
// each event that comes after the fetch's end, and each response handed over. It is destroyed with the fetch's engine
// record, and records that too.
class EventRecordingSink final : public Web::Fetch::Engine::EngineEventSink {
public:
    EventRecordingSink(NonnullOwnPtr<Web::Fetch::Engine::EngineEventSink> event_sink, NonnullRefPtr<ReachedSteps> reached, NonnullRefPtr<ReachedSteps> records_gone, NonnullRefPtr<HandedOverResponses> handed_over, ByteString fetch)
        : m_event_sink(move(event_sink))
        , m_reached(move(reached))
        , m_records_gone(move(records_gone))
        , m_handed_over(move(handed_over))
        , m_fetch(move(fetch))
    {
    }

    virtual ~EventRecordingSink() override
    {
        m_records_gone->steps.append(m_fetch);
    }

    virtual void handle_event(Web::Fetch::Engine::EngineEvent event) override
    {
        if (m_fetch_ended)
            m_reached->steps.append(ByteString::formatted("event after the end of fetch {}", m_fetch));
        else if (event.has<Web::Fetch::Engine::Event::EndOfBody>())
            m_reached->steps.append(ByteString::formatted("end of body of {}", m_fetch));
        else if (event.has<Web::Fetch::Engine::Event::FetchEnded>())
            m_reached->steps.append(ByteString::formatted("end of fetch {}", m_fetch));
        if (auto const* delivery = event.get_pointer<Web::Fetch::Engine::Event::ResponseDelivery>())
            m_handed_over->responses.append(delivery->response);
        m_fetch_ended |= event.has<Web::Fetch::Engine::Event::FetchEnded>();
        m_event_sink->handle_event(move(event));
    }

private:
    NonnullOwnPtr<Web::Fetch::Engine::EngineEventSink> m_event_sink;
    NonnullRefPtr<ReachedSteps> m_reached;
    NonnullRefPtr<ReachedSteps> m_records_gone;
    NonnullRefPtr<HandedOverResponses> m_handed_over;
    ByteString m_fetch;
    bool m_fetch_ended { false };
};

class IgnoringEventSink final : public Web::Fetch::Engine::EngineEventSink {
public:
    virtual void handle_event(Web::Fetch::Engine::EngineEvent) override { }
};

// A load of a fetch that an engine of its own starts, for a transport that a case drives itself.
inline NonnullRefPtr<Web::Fetch::Engine::FetchLoad> start_a_load_of_another_engine()
{
    Web::Fetch::Engine::FetchEngineRecords records { make<Web::Fetch::Engine::FetchEngineRecords::Agent>() };
    auto record = records.start(Web::Fetch::Engine::FetchId { 1 }, make<IgnoringEventSink>(), Web::Fetch::Engine::NetworkActivityObservers::create());
    return record->start_load();
}

// Records each command that reaches the fetch engine, then hands it to a fetch engine through its executor, keeps it
// for the test to hand on, or drops it. A fetch the recorder saw start is named by its number in the order of the
// starts, from 1, and any other fetch by its id. The fetch engine's events record the ends of the bodies of the
// fetches it starts, and of the fetches.
class CommandRecordingEngine final : public Web::Fetch::Engine::EngineCommandSink {
public:
    using Destination = Variant<Empty, NonnullRefPtr<PostedCommands>, NonnullRefPtr<Web::Fetch::Engine::FetchExecutor>>;

    explicit CommandRecordingEngine(NonnullRefPtr<ReachedSteps> reached, Destination destination = Empty {}, RefPtr<ReachedSteps> records_gone = {}, RefPtr<HandedOverResponses> handed_over = {})
        : m_reached(move(reached))
        , m_records_gone(records_gone ? records_gone.release_nonnull() : make_ref_counted<ReachedSteps>())
        , m_handed_over(handed_over ? handed_over.release_nonnull() : make_ref_counted<HandedOverResponses>())
        , m_destination(move(destination))
    {
    }

    virtual void attach_to_executor(Web::Fetch::Engine::FetchExecutor&) override { }

    virtual void handle_command(Web::Fetch::Engine::EngineCommand command) override
    {
        using namespace Web::Fetch::Engine::Command;
        m_reached->steps.append(describe(command));
        if (auto const* engine_executor = m_destination.get_pointer<NonnullRefPtr<Web::Fetch::Engine::FetchExecutor>>()) {
            if (auto* start = command.get_pointer<StartFetch>())
                start->input.event_sink = make<EventRecordingSink>(move(start->input.event_sink), m_reached, m_records_gone, m_handed_over, fetch(start->input.fetch_id));
            (*engine_executor)->post(move(command));
            return;
        }

        // Without a fetch engine, a case expects only stops, transport controls and adopted navigation bodies.
        if (!command.has<Abort>() && !command.has<Terminate>() && !command.has<Stop>() && !command.has<TransportControl>() && !command.has<NavigationBodyAdopted>())
            FAIL("unexpected command");
        if (auto const* posted_commands = m_destination.get_pointer<NonnullRefPtr<PostedCommands>>())
            (*posted_commands)->commands.append(move(command));
    }

private:
    ByteString fetch(Web::Fetch::Engine::FetchId fetch_id) const
    {
        if (auto index = m_started_fetches.find_first_index(fetch_id); index.has_value())
            return ByteString::number(*index + 1);
        return ByteString::number(fetch_id.value());
    }

    ByteString describe(Web::Fetch::Engine::EngineCommand const& command)
    {
        using namespace Web::Fetch::Engine::Command;
        return command.visit(
            [&](StartFetch const& start) {
                m_started_fetches.append(start.input.fetch_id);
                return ByteString::formatted("start fetch {}", fetch(start.input.fetch_id));
            },
            [&](Abort const& abort) { return ByteString::formatted("abort {}", fetch(abort.fetch_id)); },
            [&](Terminate const& terminate) { return ByteString::formatted("terminate {}", fetch(terminate.fetch_id)); },
            [&](Stop const& stop) { return ByteString::formatted("stop {}", fetch(stop.fetch_id)); },
            [&](ResolveRedirect const& resolve) {
                return resolve.resolution.visit(
                    [&](ResolveRedirect::Follow) { return ByteString::formatted("follow redirect of {}", fetch(resolve.fetch_id)); },
                    [&](ResolveRedirect::AcceptAsFinal) { return ByteString::formatted("accept redirect of {} as final", fetch(resolve.fetch_id)); },
                    [&](ResolveRedirect::Abandon) { return ByteString::formatted("abandon redirect of {}", fetch(resolve.fetch_id)); });
            },
            [&](PreloadResponseResolved const& resolved) { return ByteString::formatted("preload response resolved {}", fetch(resolved.fetch_id)); },
            [&](PreloadWaitAbandoned const& abandoned) { return ByteString::formatted("abandon preload wait of {}", fetch(abandoned.fetch_id)); },
            [](TransportControl const& control) { return describe_transport_control(control.operation, control.request_id, control.transport_epoch); },
            [](NavigationBodyAdopted const&) { return ByteString { "navigation body adopted"sv }; },
            [](AdoptNetworkTransport const&) { return ByteString { "adopt network transport"sv }; });
    }

    NonnullRefPtr<ReachedSteps> m_reached;
    NonnullRefPtr<ReachedSteps> m_records_gone;
    NonnullRefPtr<HandedOverResponses> m_handed_over;
    Destination m_destination;
    Vector<Web::Fetch::Engine::FetchId> m_started_fetches;
};

// Runs the agent's tasks until the condition holds, or for the given time.
inline void run_tasks_until(GC::Heap& heap, GC::Ref<GC::Function<bool()>> condition, AK::Duration timeout = AK::Duration::from_seconds(10))
{
    auto deadline = Core::Timer::create_single_shot(timeout, [] { });
    deadline->start();
    Web::HTML::main_thread_event_loop().spin_until(GC::create_function(heap, GC::captures(condition, deadline->make_weak_ptr<Core::Timer>()), [](auto& condition, auto& deadline) {
        return condition->function()() || !deadline || !deadline->is_active();
    }));
}

struct FetchHarness {
    FetchHarness()
        : document(create_test_document())
        , realm(document->relevant_settings_object().realm())
    {
        // Each case has an event loop of its own, which the window agent's fetches have to run on.
        auto& agent = Web::HTML::surrounding_agent();
        agent.fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor(agent.network_activity_observers);
    }

    // Records each command the agent posts from now on, before the agent's fetch engine gets it, the fetches whose
    // engine records are gone in records_gone, and the responses the engine hands over in handed_over.
    NonnullRefPtr<ReachedSteps> record_commands() const
    {
        auto commands = make_ref_counted<ReachedSteps>();
        auto& agent = Web::HTML::surrounding_agent();
        Web::Fetch::Engine::FetchExecutorServices services { .event_loop = Core::EventLoop::current_weak() };
        auto recording_engine = make<CommandRecordingEngine>(commands, NonnullRefPtr { *agent.fetch_executor }, records_gone, handed_over);
        agent.fetch_executor = Web::Fetch::Engine::InlineFetchExecutor::create(move(services), move(recording_engine));
        return commands;
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

    // A request like the one a navigation fetches, whose fetch hands each redirect over without following it.
    NonnullRefPtr<Web::Fetch::Infrastructure::Request> create_navigation_request_for(StringView url_string) const
    {
        using Web::Fetch::Infrastructure::Request;
        auto request = create_request_for(url_string);
        request->set_mode(Request::Mode::Navigate);
        request->set_destination(Request::Destination::Document);
        request->set_redirect_mode(Request::RedirectMode::Manual);
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

    // Fetches as a navigation does: on a parallel queue, with a transfer lease on each response's body.
    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch_as_a_navigation(Web::Fetch::Infrastructure::Request& request, Web::Fetch::Infrastructure::FetchAlgorithms::Input input) const
    {
        Web::HTML::TemporaryExecutionContext execution_context { *realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto algorithms = Web::Fetch::Infrastructure::FetchAlgorithms::create(move(input));
        return Web::Fetch::Fetching::fetch(*realm, request, algorithms, Web::Fetch::Fetching::UseParallelQueue::Yes, Web::Fetch::Fetching::CreateResponseBodyTransferLease::Yes);
    }

    void with_execution_context(Function<void()> steps) const
    {
        Web::HTML::TemporaryExecutionContext execution_context { *realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps();
    }

    // The event loop runs no task while the handle lives.
    Web::HTML::EventLoop::PauseHandle pause_event_loop() const
    {
        Web::HTML::TemporaryExecutionContext execution_context { *realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        return Web::HTML::main_thread_event_loop().pause(Web::HTML::EventLoop::UpdateTheRendering::No);
    }

    // Runs the fetch's steps and the tasks they queue until the condition holds, or for the given time.
    void run_tasks_until(GC::Ref<GC::Function<bool()>> condition, AK::Duration timeout = AK::Duration::from_seconds(10)) const
    {
        FetchTest::run_tasks_until(realm->heap(), condition, timeout);
    }

    void run_until_step(NonnullRefPtr<ReachedSteps> const& reached, StringView step) const
    {
        auto expected = ByteString { step };
        run_tasks_until(GC::create_function(realm->heap(), GC::captures(reached, expected), [](auto& reached, auto& expected) { return reached->steps.contains_slow(expected); }));
    }

    void run_until_responses(NonnullRefPtr<AlgorithmCalls> const& record, size_t count) const
    {
        run_tasks_until(GC::create_function(realm->heap(), GC::captures(record, count), [](auto& record, auto& count) { return record->responses.size() == count; }));
        VERIFY(record->responses.size() == count);
    }

    // Runs everything for a while. What has not happened by then is taken not to happen.
    void settle(AK::Duration duration = AK::Duration::from_milliseconds(300)) const
    {
        run_tasks_until(GC::create_function(realm->heap(), [] { return false; }), duration);
    }

    // Runs the deferred invocations queued so far, then those that they queue.
    void run_queued_invocations() const
    {
        auto reached = make_ref_counted<ReachedSteps>();
        Core::deferred_invoke([reached] {
            Core::deferred_invoke([reached] { reached->marker_reached = true; });
        });
        run_tasks_until(GC::create_function(realm->heap(), GC::captures(reached), [](auto& reached) { return reached->marker_reached; }));
    }

    GC::Ref<Web::DOM::Document> document;
    GC::Ref<JS::Realm> realm;
    NonnullRefPtr<ReachedSteps> records_gone { make_ref_counted<ReachedSteps>() };
    NonnullRefPtr<HandedOverResponses> handed_over { make_ref_counted<HandedOverResponses>() };
};

// What a RequestServer the test stands in for has been sent: each message, in the order it came, as
// "start <id> <method> <path>", "stop <id>", "release lease <id>", or the message's name.
struct RequestServerMessages : RefCounted<RequestServerMessages> {
    GC_HOLDS_NO_CELL_EDGES;

    void read()
    {
        (void)transport->read_as_many_messages_as_possible_without_blocking([this](auto&& raw_message) {
            auto message = MUST(RequestServerEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
            auto message_id = message->message_id();
            if (message_id == Messages::RequestServer::StartRequest::static_message_id()) {
                auto const& start = static_cast<Messages::RequestServer::StartRequest&>(*message);
                started_ids.append(start.request_id());
                started_methods.append(start.method());
                messages.append(ByteString::formatted("start {} {} {}", start.request_id(), start.method(), start.url().serialize_path()));
            } else if (message_id == Messages::RequestServer::StopRequest::static_message_id()) {
                auto const& stop = static_cast<Messages::RequestServer::StopRequest&>(*message);
                messages.append(ByteString::formatted("stop {}", stop.request_id()));
            } else if (message_id == Messages::RequestServer::ReleaseRequestTransferLease::static_message_id()) {
                auto const& release = static_cast<Messages::RequestServer::ReleaseRequestTransferLease&>(*message);
                messages.append(ByteString::formatted("release lease {}", release.source_request_id()));
            } else {
                messages.append(message->message_name());
            }
        });
    }

    IPC::Transport* transport { nullptr };
    Vector<ByteString> messages;
    Vector<u64> started_ids;
    Vector<ByteString> started_methods;
};

// A RequestServer client of the agent's resource loader, whose RequestServer end of the connection the test holds: it
// records each message RequestServer gets, and answers only as the test says.
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
        m_received->transport = m_request_server.ptr();
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
        m_harness.run_tasks_until(GC::create_function(m_harness.realm->heap(), GC::captures(m_received, m_taken_count, expected), [](auto& received, auto& taken_count, auto& expected) {
            received->read();
            for (size_t index = taken_count; index < received->started_methods.size(); ++index) {
                if (received->started_methods[index] == expected)
                    return true;
            }
            return false;
        }));
        for (size_t index = m_taken_count; index < m_received->started_methods.size(); ++index) {
            if (m_received->started_methods[index] == method) {
                m_taken_count = index + 1;
                return m_received->started_ids[index];
            }
        }
        VERIFY_NOT_REACHED();
    }

    // Runs the agent's tasks until RequestServer has been sent the message, named as messages() names it.
    void wait_for_message(StringView message)
    {
        auto wake_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { });
        wake_timer->start();

        auto expected = ByteString { message };
        m_harness.run_tasks_until(GC::create_function(m_harness.realm->heap(), GC::captures(m_received, expected), [](auto& received, auto& expected) {
            received->read();
            return received->messages.contains_slow(expected);
        }));
    }

    // The messages RequestServer has been sent so far.
    Vector<ByteString> const& messages() const
    {
        m_received->read();
        return m_received->messages;
    }

    void send(IPC::MessageBuffer message)
    {
        MUST(message.transfer_message(*m_request_server));
    }

    void send_headers(u64 request_id, u32 status_code, Vector<HTTP::Header> headers = {})
    {
        send(MUST(Messages::RequestClient::HeadersBecameAvailable::static_encode(request_id, URL::about_blank(), move(headers), status_code, Optional<String> {}, Requests::CacheState::NotCached, ByteString {})));
    }

    void send_finished(u64 request_id, Requests::RequestTimingInfo timing_info = {}, Optional<Requests::NetworkError> network_error = {})
    {
        send(MUST(Messages::RequestClient::RequestFinished::static_encode(request_id, 0, timing_info, network_error)));
    }

    // Delivers the body the file holds, as RequestServer does for a body it keeps in a file.
    void send_body_file(u64 request_id, ByteString const& path, size_t size)
    {
        auto file = IPC::File::adopt_file(MUST(Core::File::open(path, Core::File::OpenMode::Read)));
        send(MUST(Messages::RequestClient::RequestBodyFileAvailable::static_encode(request_id, file, 0, size)));
    }

    void send_transferred(u64 request_id)
    {
        send(MUST(Messages::RequestClient::RequestTransferred::static_encode(request_id)));
    }

    // Has the agent's client handle every message sent so far, one after another, before anything else runs.
    void deliver_sent_messages()
    {
        m_request_server->flush();
        m_client->dispatch_pending_messages();
    }

private:
    FetchHarness const& m_harness;
    RefPtr<Requests::RequestClient> m_client;
    OwnPtr<IPC::Transport> m_request_server;
    NonnullRefPtr<RequestServerMessages> m_received { make_ref_counted<RequestServerMessages>() };
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

// The token of the agent's fetch whose fetch params match, which a test holds instead of the fetch's controller.
inline NEVER_INLINE Optional<Web::Fetch::Infrastructure::FetchControllerToken> token_of_fetch(Function<bool(Web::Fetch::Infrastructure::FetchParams const&)> const& matches)
{
    for (auto const& controller : Web::HTML::surrounding_agent().fetch_controller_registry.controllers()) {
        auto fetch_params = controller->original_fetch_params();
        if (fetch_params && matches(*fetch_params))
            return controller->token();
    }
    return {};
}

// The token of the fetch whose request's first URL has the path.
inline Optional<Web::Fetch::Infrastructure::FetchControllerToken> token_of_fetch_for(StringView path)
{
    return token_of_fetch([&](auto const& fetch_params) { return fetch_params.request()->url().serialize_path() == path; });
}

inline NEVER_INLINE GC::Ptr<Web::Fetch::Infrastructure::FetchController> find_controller(Web::Fetch::Infrastructure::FetchControllerToken token)
{
    return Web::HTML::surrounding_agent().fetch_controller_registry.find(token);
}

// Whether the fetch has handed a response over, whose process response task the handover queued.
inline bool was_handed_over(Web::Fetch::Infrastructure::FetchController const& controller)
{
    return controller.control_block().agent_timing_info();
}

// Whether the agent's registry keeps the controller alive, as that of a fetch or of a fetch's CORS preflight.
inline bool is_rooted(Web::Fetch::Infrastructure::FetchControllerToken token)
{
    return Web::HTML::surrounding_agent().fetch_controller_registry.roots(token);
}

}
