/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineEvent.h>
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

using namespace Web::Fetch::Engine;

// A command is handed to the engine exactly once.
static_assert(!IsCopyConstructible<EngineCommand>);
static_assert(IsMoveConstructible<EngineCommand>);

// A delivered body is never null.
static_assert(!IsConstructible<Delivered<NonNullLocalBodyHandle>, NullBody>);

// Only fetch params build the input that starts a fetch.
static_assert(!IsConstructible<StartFetchInput, FetchId, MainFetchInput, NonnullOwnPtr<EngineEventSink>>);

TEST_CASE(commands_round_trip_their_payloads)
{
    Vector<EngineCommand> commands;
    commands.append(Command::Abort { .fetch_id = FetchId(2) });
    commands.append(Command::Terminate { .fetch_id = FetchId(3) });
    commands.append(Command::Stop { .fetch_id = FetchId(4) });
    commands.append(Command::ResolveRedirect { .fetch_id = FetchId(5), .continuation_id = ContinuationId(6), .resolution = Command::ResolveRedirect::AcceptAsFinal {} });
    auto preloaded_response = Web::Fetch::Infrastructure::Response::create();
    commands.append(Command::PreloadResponseResolved { .fetch_id = FetchId(7), .response = preloaded_response });
    ModifyRequest modified_request;
    modified_request.url = URL::Parser::basic_parse("https://example.com/modified"sv);
    modified_request.method = "PUT";
    modified_request.headers = Vector<HTTP::Header> { { "X-Modified"sv, "1"sv } };
    modified_request.cookie = "a=b";
    modified_request.body = MUST(ByteBuffer::copy("modified"sv.bytes()));
    commands.append(Command::ResolveNetworkInterception { .fetch_id = FetchId(10), .continuation_id = ContinuationId(11), .decision = move(modified_request) });
    commands.append(Command::TransportControl { .request_id = RequestId(8), .transport_epoch = TransportEpoch(9), .operation = Command::TransportControl::ReleaseNavigationHoldUpTo { .byte_count = 4096 } });

    Vector<u64> seen_ids;
    for (auto& command : commands) {
        command.visit(
            [&](Command::StartFetch const&) { FAIL("no fetch was started"); },
            [&](Command::Abort const& abort) { seen_ids.append(abort.fetch_id.value()); },
            [&](Command::Terminate const& terminate) { seen_ids.append(terminate.fetch_id.value()); },
            [&](Command::Stop const& stop) { seen_ids.append(stop.fetch_id.value()); },
            [&](Command::ResolveRedirect const& resolve) {
                EXPECT(resolve.resolution.has<Command::ResolveRedirect::AcceptAsFinal>());
                EXPECT_EQ(resolve.continuation_id, ContinuationId(6));
                seen_ids.append(resolve.fetch_id.value());
            },
            [&](Command::PreloadResponseResolved const& preload) {
                EXPECT_EQ(preload.response.ptr(), preloaded_response.ptr());
                seen_ids.append(preload.fetch_id.value());
            },
            [&](Command::ResolveNetworkInterception const& resolve) {
                EXPECT_EQ(resolve.continuation_id, ContinuationId(11));
                auto const& modified = resolve.decision.get<ModifyRequest>();
                EXPECT_EQ(modified.url->serialize(), "https://example.com/modified"sv);
                EXPECT_EQ(modified.method, "PUT"sv);
                VERIFY(modified.headers.has_value() && modified.headers->size() == 1);
                EXPECT_EQ(modified.headers->first().name, "X-Modified"sv);
                EXPECT_EQ(modified.cookie, "a=b"sv);
                EXPECT_EQ(modified.body->bytes(), "modified"sv.bytes());
                seen_ids.append(resolve.fetch_id.value());
            },
            [&](Command::TransportControl const& control) {
                EXPECT_EQ(control.transport_epoch, TransportEpoch(9));
                EXPECT_EQ(control.operation.get<Command::TransportControl::ReleaseNavigationHoldUpTo>().byte_count, 4096u);
                seen_ids.append(control.request_id.value());
            },
            [&](Command::NavigationBodyAdopted const&) { FAIL("no navigation body was adopted"); },
            [&](Command::AdoptNetworkTransport const&) { FAIL("no transport was posted"); });
    }
    EXPECT_EQ(seen_ids, (Vector<u64> { 2, 3, 4, 5, 7, 10, 8 }));
}

static bool peer_has_hung_up(IPC::Transport& transport)
{
    return transport.read_as_many_messages_as_possible_without_blocking([](auto&&) { }) == IPC::Transport::ShouldShutdown::Yes;
}

static void wait_for_peer_to_hang_up(IPC::Transport& transport)
{
    while (!peer_has_hung_up(transport))
        transport.wait_until_readable();
}

TEST_CASE(adopt_network_transport_owns_its_handle)
{
    auto pair = MUST(IPC::Transport::create_paired());

    Optional<EngineCommand> command = Command::AdoptNetworkTransport { .handle = move(pair.remote_handle) };
    EXPECT(!peer_has_hung_up(*pair.local));

    // The moved-to command alone owns the handle.
    EngineCommand moved = command.release_value();
    EXPECT(!peer_has_hung_up(*pair.local));

    moved = Command::Stop { .fetch_id = FetchId(1) };
    wait_for_peer_to_hang_up(*pair.local);
}

TEST_CASE(delivered_body_requires_a_valid_handle)
{
    EXPECT_DEATH("a delivered body is never null", (void)Delivered<NonNullLocalBodyHandle> { Core::ImmutableBytes {} });
}

namespace {

class IgnoringProducerSink final : public FetchByteChannelProducerSink {
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
};

}

TEST_CASE(delivered_body_carries_a_channel_still_being_written)
{
    auto channel = FetchByteChannel::create({ .low = 1, .high = 2 }, adopt_ref(*new IgnoringProducerSink));
    LocalBodyPresentation body = Delivered<NonNullLocalBodyHandle> { channel };

    auto const& delivered = body.get<Delivered<NonNullLocalBodyHandle>>();
    EXPECT(delivered.handle().has<NonnullRefPtr<FetchByteChannel>>());
    EXPECT_EQ(delivered.handle().get<NonnullRefPtr<FetchByteChannel>>().ptr(), channel.ptr());
}

TEST_CASE(events_round_trip_their_payloads)
{
    FetchTimingSeed timing_seed;
    timing_seed.start_time = 12.5;
    timing_seed.render_blocking = true;

    auto response = Web::Fetch::Infrastructure::Response::create();
    response->set_status(201);

    Vector<EngineEvent> events;
    events.append(Event::ResponseDelivery {
        .response = response,
        .body = NullBody {},
        .timing_seed = move(timing_seed),
        .redirect_continuation_id = ContinuationId(1),
    });
    events.append(Event::EndOfBody { .delivery_id = DeliveryId(2), .end_time = 99 });
    events.append(Event::ConsumeBodyResult { .delivery_id = DeliveryId(3), .outcome = Event::ConsumeBodyResult::Bytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("body"sv.bytes()))) } });
    events.append(Event::BodyChannelReadable { .delivery_id = DeliveryId(4), .ticket = WakeTicket(7) });
    events.append(Event::DeliveryEnd { .delivery_id = DeliveryId(5), .outcome = Event::DeliveryEnd::Revoked {} });
    events.append(Event::RequestTransmissionStarted { .transmission_id = TransmissionId(6), .generation = TransmissionGeneration(1) });
    Web::Fetch::Infrastructure::ConnectionTimingInfo connection_timing_info;
    connection_timing_info.connection_start_time = 3;
    connection_timing_info.alpn_negotiated_protocol = "h2";
    events.append(Event::RequestTransmissionEnded {
        .network_phases = {
            .final_connection_timing_info = move(connection_timing_info),
            .final_network_request_start_time = 4,
            .final_network_response_start_time = 5,
        },
    });
    events.append(Event::RequestBodyProgress { .length = 8 });
    events.append(Event::RequestBodyEndOfBody { .body_is_null = true });
    events.append(Event::ReadRequestBodyStream {});
    events.append(Event::RequestBodySentFromSource {});
    events.append(Event::TransportActivity {
        .request_id = RequestId(8),
        .activity = Event::TransportActivity::RequestStarted {
            .url = URL::Parser::basic_parse("https://example.com/upload"sv).release_value(),
            .method = "POST",
            .headers = { { "Accept"sv, "*/*"sv } },
            .body = Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("request"sv.bytes()))),
            .initiator_type = "fetch"_string,
            .referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::NoReferrer,
            .is_navigation_request = true,
            .priority = Web::Fetch::Infrastructure::RequestPriority::High,
        },
    });
    events.append(Event::TransportActivity {
        .request_id = RequestId(9),
        .activity = Event::TransportActivity::ResponseHeaders {
            .status_code = 203,
            .reason_phrase = "Non-Authoritative"_string,
            .headers = { { "Content-Type"sv, "text/plain"sv } },
            .cache_state = Requests::CacheState::Validated,
        },
    });
    events.append(Event::TransportActivity {
        .request_id = RequestId(10),
        .activity = Event::TransportActivity::ResponseBodyBytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("response"sv.bytes()))) },
    });
    events.append(Event::TransportActivity {
        .request_id = RequestId(11),
        .activity = Event::TransportActivity::RequestFinished {
            .total_size = 8,
            .timing_info = { .request_start_microseconds = 40, .encoded_body_size = 8 },
            .network_error = Requests::NetworkError::IncompleteContent,
        },
    });
    Event::ObservedResponse observed_response {
        .url = URL::Parser::basic_parse("https://example.com/observed"sv),
        .status = 401,
        .status_message = "Unauthorized",
        .headers = { { "WWW-Authenticate"sv, "Basic realm=\"realm\""sv } },
        .protocol = "http/1.1",
        .from_cache = true,
        .encoded_body_size = 12,
        .decoded_body_size = 34,
    };
    Vector<Event::FetchNetworkActivity::Activity> fetch_activities;
    fetch_activities.append(Event::FetchNetworkActivity::BeforeRequestSent { .interception = ContinuationId(20) });
    fetch_activities.append(Event::FetchNetworkActivity::ResponseStarted { .response = observed_response, .interception = {} });
    fetch_activities.append(Event::FetchNetworkActivity::AuthRequired { .response = observed_response, .interception = ContinuationId(21) });
    fetch_activities.append(Event::FetchNetworkActivity::RequestUpdated {});
    fetch_activities.append(Event::FetchNetworkActivity::ResponseCompleted { observed_response });
    fetch_activities.append(Event::FetchNetworkActivity::Failed { "Failed"_string });
    for (auto& activity : fetch_activities) {
        Web::Fetch::Infrastructure::ConnectionTimingInfo observed_connection_timing_info;
        observed_connection_timing_info.domain_lookup_start_time = 6;
        events.append(Event::FetchNetworkActivity {
            .cors_preflight_id = CorsPreflightId(12),
            .request = {
                .url = URL::Parser::basic_parse("https://example.com/observed"sv).release_value(),
                .method = "PATCH",
                .headers = { { "X-Observed"sv, "1"sv } },
                .body_size = 3,
                .destination = "script"_string,
                .initiator_type = "fetch"_string,
                .redirect_count = 2,
                .navigable_id = Web::HTML::CrossProcessId { .namespace_id = 5, .local_id = 6 },
                .navigation_id = "navigation"_string,
                .time_origin = 7.5,
                .timing_seed = { .start_time = 8, .redirect_start_time = 9, .redirect_end_time = 10, .post_redirect_start_time = 11, .final_service_worker_start_time = 0, .server_timing_headers = { "total;dur=1"_string }, .render_blocking = true },
                .network_phases = FetchNetworkPhases {
                    .final_connection_timing_info = move(observed_connection_timing_info),
                    .final_network_request_start_time = 13,
                    .final_network_response_start_time = 14,
                },
            },
            .activity = move(activity),
        });
    }

    auto policy = Web::ContentSecurityPolicy::Policy::parse_a_serialized_csp("img-src 'none'"_string, Web::ContentSecurityPolicy::Policy::Source::Header, Web::ContentSecurityPolicy::Policy::Disposition::Enforce);
    events.append(Event::ViolationReport {
        .violation = {
            .policy = policy,
            .effective_directive = "img-src"_utf16_fly_string,
            .resource = URL::Parser::basic_parse("https://example.com/image"sv).release_value(),
            .reported_to = Web::ContentSecurityPolicy::RequestViolation::RemoteClient {
                .policy_index = 2,
                .navigation = Web::HTML::RemoteClientNavigation { .navigable_id = { .namespace_id = 3, .local_id = 4 }, .navigation_id = "navigation"_utf16 },
            },
        },
    });
    events.append(Event::FetchEnded {});

    Vector<u64> seen_ids;
    bool seen_network_phases = false;
    bool seen_violation_report = false;
    bool seen_fetch_end = false;
    Vector<StringView> seen_request_body_events;
    Vector<u64> seen_network_request_ids;
    Vector<StringView> seen_fetch_activities;
    Vector<u64> seen_interceptions;
    auto expect_observed_response = [](Event::ObservedResponse const& response) {
        EXPECT_EQ(response.url->serialize(), "https://example.com/observed"sv);
        EXPECT_EQ(response.status, 401);
        EXPECT_EQ(response.status_message, "Unauthorized"sv);
        VERIFY(response.headers.size() == 1);
        EXPECT_EQ(response.headers[0].name, "WWW-Authenticate"sv);
        EXPECT_EQ(response.protocol, "http/1.1"sv);
        EXPECT(response.from_cache);
        EXPECT_EQ(response.encoded_body_size, 12u);
        EXPECT_EQ(response.decoded_body_size, 34u);
    };
    for (auto const& event : events) {
        event.visit(
            [&](Event::ResponseDelivery const& delivery) {
                EXPECT_EQ(delivery.response.ptr(), response.ptr());
                EXPECT_EQ(delivery.response->status(), 201);
                EXPECT(delivery.body.has<NullBody>());
                EXPECT_EQ(delivery.timing_seed.start_time, 12.5);
                EXPECT(delivery.timing_seed.render_blocking);
                seen_ids.append(delivery.redirect_continuation_id->value());
            },
            [&](Event::EndOfBody const& end) {
                EXPECT_EQ(end.end_time, 99);
                seen_ids.append(end.delivery_id.value());
            },
            [&](Event::ConsumeBodyResult const& result) {
                EXPECT_EQ(result.outcome.get<Event::ConsumeBodyResult::Bytes>().bytes.bytes(), "body"sv.bytes());
                seen_ids.append(result.delivery_id.value());
            },
            [&](Event::BodyChannelReadable const& readable) {
                EXPECT_EQ(readable.ticket, WakeTicket(7));
                seen_ids.append(readable.delivery_id.value());
            },
            [&](Event::DeliveryEnd const& delivery_end) {
                EXPECT(delivery_end.outcome.has<Event::DeliveryEnd::Revoked>());
                seen_ids.append(delivery_end.delivery_id.value());
            },
            [&](Event::RequestTransmissionStarted const& started) { seen_ids.append(started.transmission_id.value()); },
            [&](Event::RequestTransmissionEnded const& ended) {
                auto const& phases = ended.network_phases;
                EXPECT_EQ(phases.final_connection_timing_info.connection_start_time, 3);
                EXPECT_EQ(phases.final_connection_timing_info.alpn_negotiated_protocol, "h2"sv);
                EXPECT_EQ(phases.final_network_request_start_time, 4);
                EXPECT_EQ(phases.final_network_response_start_time, 5);
                seen_network_phases = true;
            },
            [&](Event::RequestBodyProgress const& progress) {
                EXPECT_EQ(progress.length, 8u);
                seen_request_body_events.append("progress"sv);
            },
            [&](Event::RequestBodyEndOfBody const& end_of_body) {
                EXPECT(end_of_body.body_is_null);
                seen_request_body_events.append("end of body"sv);
            },
            [&](Event::ReadRequestBodyStream const&) { seen_request_body_events.append("read stream"sv); },
            [&](Event::RequestBodySentFromSource const&) { seen_request_body_events.append("sent from source"sv); },
            [&](Event::TransportActivity const& network_activity) {
                network_activity.activity.visit(
                    [&](Event::TransportActivity::RequestStarted const& started) {
                        EXPECT_EQ(started.url.serialize(), "https://example.com/upload"sv);
                        EXPECT_EQ(started.method, "POST"sv);
                        VERIFY(started.headers.size() == 1);
                        EXPECT_EQ(started.headers[0].name, "Accept"sv);
                        EXPECT_EQ(started.headers[0].value, "*/*"sv);
                        EXPECT_EQ(started.body.bytes(), "request"sv.bytes());
                        EXPECT_EQ(started.initiator_type, "fetch"sv);
                        EXPECT_EQ(started.referrer_policy, Web::ReferrerPolicy::ReferrerPolicy::NoReferrer);
                        EXPECT(started.is_navigation_request);
                        EXPECT_EQ(started.priority, Web::Fetch::Infrastructure::RequestPriority::High);
                    },
                    [&](Event::TransportActivity::ResponseHeaders const& headers) {
                        EXPECT_EQ(headers.status_code, 203u);
                        EXPECT_EQ(headers.reason_phrase, "Non-Authoritative"sv);
                        VERIFY(headers.headers.size() == 1);
                        EXPECT_EQ(headers.headers[0].value, "text/plain"sv);
                        EXPECT_EQ(headers.cache_state, Requests::CacheState::Validated);
                    },
                    [&](Event::TransportActivity::ResponseBodyBytes const& body_bytes) {
                        EXPECT_EQ(body_bytes.bytes.bytes(), "response"sv.bytes());
                    },
                    [&](Event::TransportActivity::RequestFinished const& finished) {
                        EXPECT_EQ(finished.total_size, 8u);
                        EXPECT_EQ(finished.timing_info.request_start_microseconds, 40);
                        EXPECT_EQ(finished.timing_info.encoded_body_size, 8);
                        EXPECT(finished.network_error == Requests::NetworkError::IncompleteContent);
                    });
                seen_network_request_ids.append(network_activity.request_id.value());
            },
            [&](Event::FetchNetworkActivity const& activity) {
                EXPECT_EQ(activity.cors_preflight_id, CorsPreflightId(12));
                auto const& request = activity.request;
                EXPECT_EQ(request.url.serialize(), "https://example.com/observed"sv);
                EXPECT_EQ(request.method, "PATCH"sv);
                VERIFY(request.headers.size() == 1);
                EXPECT_EQ(request.headers[0].name, "X-Observed"sv);
                EXPECT_EQ(request.body_size, 3u);
                EXPECT_EQ(request.destination, "script"sv);
                EXPECT_EQ(request.initiator_type, "fetch"sv);
                EXPECT_EQ(request.redirect_count, 2);
                EXPECT_EQ(request.navigable_id, (Web::HTML::CrossProcessId { .namespace_id = 5, .local_id = 6 }));
                EXPECT_EQ(request.navigation_id, "navigation"sv);
                EXPECT_EQ(request.time_origin, 7.5);
                EXPECT_EQ(request.timing_seed.start_time, 8);
                EXPECT_EQ(request.timing_seed.redirect_start_time, 9);
                EXPECT_EQ(request.timing_seed.redirect_end_time, 10);
                EXPECT_EQ(request.timing_seed.post_redirect_start_time, 11);
                EXPECT_EQ(request.timing_seed.server_timing_headers, (Vector<String> { "total;dur=1"_string }));
                EXPECT(request.timing_seed.render_blocking);
                VERIFY(request.network_phases.has_value());
                EXPECT_EQ(request.network_phases->final_connection_timing_info.domain_lookup_start_time, 6);
                EXPECT_EQ(request.network_phases->final_network_request_start_time, 13);
                EXPECT_EQ(request.network_phases->final_network_response_start_time, 14);
                activity.activity.visit(
                    [&](Event::FetchNetworkActivity::BeforeRequestSent const& reached) {
                        seen_interceptions.append(reached.interception->value());
                        seen_fetch_activities.append("before request sent"sv);
                    },
                    [&](Event::FetchNetworkActivity::ResponseStarted const& reached) {
                        EXPECT(!reached.interception.has_value());
                        expect_observed_response(reached.response);
                        seen_fetch_activities.append("response started"sv);
                    },
                    [&](Event::FetchNetworkActivity::AuthRequired const& reached) {
                        seen_interceptions.append(reached.interception->value());
                        expect_observed_response(reached.response);
                        seen_fetch_activities.append("auth required"sv);
                    },
                    [&](Event::FetchNetworkActivity::RequestUpdated const&) { seen_fetch_activities.append("request updated"sv); },
                    [&](Event::FetchNetworkActivity::ResponseCompleted const& completed) {
                        expect_observed_response(completed.response);
                        seen_fetch_activities.append("response completed"sv);
                    },
                    [&](Event::FetchNetworkActivity::Failed const& failed) {
                        EXPECT_EQ(failed.reason, "Failed"sv);
                        seen_fetch_activities.append("failed"sv);
                    });
            },
            [&](Event::ViolationReport const& report) {
                auto const& violation = report.violation;
                EXPECT_EQ(violation.policy.ptr(), policy.ptr());
                EXPECT_EQ(violation.effective_directive, "img-src"sv);
                EXPECT_EQ(violation.resource.serialize(), "https://example.com/image"sv);
                auto const& client = violation.reported_to.get<Web::ContentSecurityPolicy::RequestViolation::RemoteClient>();
                EXPECT_EQ(client.policy_index, 2u);
                EXPECT_EQ(client.navigation->navigable_id, (Web::HTML::CrossProcessId { .namespace_id = 3, .local_id = 4 }));
                EXPECT_EQ(client.navigation->navigation_id, "navigation"sv);
                seen_violation_report = true;
            },
            [&](Event::FetchEnded const&) { seen_fetch_end = true; });
    }
    EXPECT_EQ(seen_ids, (Vector<u64> { 1, 2, 3, 4, 5, 6 }));
    EXPECT(seen_network_phases);
    EXPECT(seen_violation_report);
    EXPECT(seen_fetch_end);
    EXPECT_EQ(seen_request_body_events, (Vector<StringView> { "progress"sv, "end of body"sv, "read stream"sv, "sent from source"sv }));
    EXPECT_EQ(seen_network_request_ids, (Vector<u64> { 8, 9, 10, 11 }));
    EXPECT_EQ(seen_fetch_activities, (Vector<StringView> { "before request sent"sv, "response started"sv, "auth required"sv, "request updated"sv, "response completed"sv, "failed"sv }));
    EXPECT_EQ(seen_interceptions, (Vector<u64> { 20, 21 }));
}

TEST_CASE(an_observed_request_keeps_the_headers_and_timing_it_was_made_with)
{
    auto request = Web::Fetch::Infrastructure::Request::create();
    request->set_url(URL::Parser::basic_parse("https://example.com/observed"sv).release_value());
    request->header_list()->append({ "X-Observed"sv, "before"sv });
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    timing_info->set_start_time(1);
    timing_info->set_network_phases({ .final_connection_timing_info = {}, .final_network_request_start_time = 2, .final_network_response_start_time = 3 });

    auto observed = Web::Fetch::Fetching::observed_request(request, timing_info);
    request->header_list()->set({ "X-Observed"sv, "after"sv });
    request->set_url(URL::Parser::basic_parse("https://example.com/changed"sv).release_value());
    timing_info->set_start_time(4);
    timing_info->set_network_phases({ .final_connection_timing_info = {}, .final_network_request_start_time = 5, .final_network_response_start_time = 6 });

    EXPECT_NE(observed.headers.data(), request->header_list()->headers().data());
    VERIFY(observed.headers.size() == 1);
    EXPECT_EQ(observed.headers[0].value, "before"sv);
    EXPECT_EQ(observed.url.serialize(), "https://example.com/observed"sv);
    EXPECT_EQ(observed.timing_seed.start_time, 1);
    VERIFY(observed.network_phases.has_value());
    EXPECT_EQ(observed.network_phases->final_network_request_start_time, 2);
    EXPECT_EQ(observed.network_phases->final_network_response_start_time, 3);
}
