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

using namespace Web::Fetch::Engine;

// A command is handed to the engine exactly once.
static_assert(!IsCopyConstructible<EngineCommand>);
static_assert(IsMoveConstructible<EngineCommand>);

// A completed preload response is all its bytes or a network error, and a delivered body is never null.
static_assert(!IsConstructible<Command::CompletedPreloadResponse::Completed, NullBody>);
static_assert(!IsConstructible<Delivered<NonNullLocalBodyHandle>, NullBody>);

// Only fetch params build the input that starts a fetch.
static_assert(!IsConstructible<StartFetchInput, FetchId, BodyIntent, Optional<Web::FileAPI::SerializedBlobURLEntry>, NonnullOwnPtr<EngineEventSink>>);

TEST_CASE(commands_round_trip_their_payloads)
{
    Vector<EngineCommand> commands;
    commands.append(Command::Abort { .fetch_id = FetchId(2) });
    commands.append(Command::Terminate { .fetch_id = FetchId(3) });
    commands.append(Command::Stop { .fetch_id = FetchId(4) });
    commands.append(Command::ResolveRedirect { .delivery_id = DeliveryId(5), .continuation_id = ContinuationId(6), .resolution = Command::ResolveRedirect::AcceptAsFinal {} });
    commands.append(Command::PreloadResponseResolved {
        .fetch_id = FetchId(7),
        .response = { .outcome = Command::CompletedPreloadResponse::Completed { .body = Delivered<Core::ImmutableBytes> { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("preloaded"sv.bytes()))) } } },
    });
    commands.append(Command::TransportControl { .request_id = RequestId(8), .transport_epoch = TransportEpoch(9), .operation = Command::TransportControl::ResumeBodyDeliveryUpTo { .byte_count = 4096 } });

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
                seen_ids.append(resolve.delivery_id.value());
            },
            [&](Command::PreloadResponseResolved const& preload) {
                auto const& completed = preload.response.outcome.get<Command::CompletedPreloadResponse::Completed>();
                EXPECT_EQ(completed.body.handle().bytes(), "preloaded"sv.bytes());
                seen_ids.append(preload.fetch_id.value());
            },
            [&](Command::TransportControl const& control) {
                EXPECT_EQ(control.transport_epoch, TransportEpoch(9));
                EXPECT_EQ(control.operation.get<Command::TransportControl::ResumeBodyDeliveryUpTo>().byte_count, 4096u);
                seen_ids.append(control.request_id.value());
            },
            [&](Command::AdoptNetworkTransport const&) { FAIL("no transport was posted"); });
    }
    EXPECT_EQ(seen_ids, (Vector<u64> { 2, 3, 4, 5, 7, 8 }));
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

    Vector<EngineEvent> events;
    events.append(Event::ResponseDelivery { .delivery_id = DeliveryId(1), .body = NullBody {}, .timing_seed = move(timing_seed) });
    events.append(Event::EndOfBody { .delivery_id = DeliveryId(2), .final_sizes = { .encoded_body_size = 10, .decoded_body_size = 20 }, .end_time = 99 });
    events.append(Event::ConsumeBodyResult { .delivery_id = DeliveryId(3), .outcome = Event::ConsumeBodyResult::Bytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("body"sv.bytes()))) } });
    events.append(Event::BodyChannelReadable { .delivery_id = DeliveryId(4), .ticket = WakeTicket(7) });
    events.append(Event::DeliveryEnd { .delivery_id = DeliveryId(5), .outcome = Event::DeliveryEnd::Revoked {} });
    events.append(Event::RequestTransmissionStarted { .transmission_id = TransmissionId(6), .generation = TransmissionGeneration(1) });
    events.append(Event::RequestTransmissionEnded {
        .transmission_id = TransmissionId(7),
        .generation = TransmissionGeneration(2),
        .request_end = {
            .request_id = RequestId(70),
            .transport_epoch = TransportEpoch(3),
            .transport_complete_at = MonotonicTime::now(),
            .outcome = RequestEnd::CommittedAttemptFailed { .attempt_id = AttemptId(4), .error = Requests::NetworkError::IncompleteContent, .encoded_body_size = 512 },
        },
    });
    events.append(Event::RequestBodyProgress { .length = 8 });
    events.append(Event::RequestBodyEndOfBody { .body_is_null = true });
    events.append(Event::ReadRequestBodyStream {});
    events.append(Event::RequestBodySentFromSource {});
    events.append(Event::NetworkActivity {
        .request_id = RequestId(8),
        .activity = Event::NetworkActivity::RequestStarted {
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
    events.append(Event::NetworkActivity {
        .request_id = RequestId(9),
        .activity = Event::NetworkActivity::ResponseHeaders {
            .status_code = 203,
            .reason_phrase = "Non-Authoritative"_string,
            .headers = { { "Content-Type"sv, "text/plain"sv } },
            .cache_state = Requests::CacheState::Validated,
        },
    });
    events.append(Event::NetworkActivity {
        .request_id = RequestId(10),
        .activity = Event::NetworkActivity::ResponseBodyBytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("response"sv.bytes()))) },
    });
    events.append(Event::NetworkActivity {
        .request_id = RequestId(11),
        .activity = Event::NetworkActivity::RequestFinished {
            .total_size = 8,
            .timing_info = { .request_start_microseconds = 40, .encoded_body_size = 8 },
            .network_error = Requests::NetworkError::IncompleteContent,
        },
    });

    Vector<u64> seen_ids;
    Vector<StringView> seen_request_body_events;
    Vector<u64> seen_network_request_ids;
    for (auto const& event : events) {
        event.visit(
            [&](Event::ResponseDelivery const& delivery) {
                EXPECT(delivery.body.has<NullBody>());
                EXPECT_EQ(delivery.timing_seed.start_time, 12.5);
                EXPECT(delivery.timing_seed.render_blocking);
                seen_ids.append(delivery.delivery_id.value());
            },
            [&](Event::EndOfBody const& end) {
                EXPECT_EQ(end.final_sizes.decoded_body_size, 20u);
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
                EXPECT_EQ(ended.request_end.request_id, RequestId(70));
                auto const& failed = ended.request_end.outcome.get<RequestEnd::CommittedAttemptFailed>();
                EXPECT_EQ(failed.error, Requests::NetworkError::IncompleteContent);
                EXPECT_EQ(failed.encoded_body_size, 512u);
                seen_ids.append(ended.transmission_id.value());
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
            [&](Event::NetworkActivity const& network_activity) {
                network_activity.activity.visit(
                    [&](Event::NetworkActivity::RequestStarted const& started) {
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
                    [&](Event::NetworkActivity::ResponseHeaders const& headers) {
                        EXPECT_EQ(headers.status_code, 203u);
                        EXPECT_EQ(headers.reason_phrase, "Non-Authoritative"sv);
                        VERIFY(headers.headers.size() == 1);
                        EXPECT_EQ(headers.headers[0].value, "text/plain"sv);
                        EXPECT_EQ(headers.cache_state, Requests::CacheState::Validated);
                    },
                    [&](Event::NetworkActivity::ResponseBodyBytes const& body_bytes) {
                        EXPECT_EQ(body_bytes.bytes.bytes(), "response"sv.bytes());
                    },
                    [&](Event::NetworkActivity::RequestFinished const& finished) {
                        EXPECT_EQ(finished.total_size, 8u);
                        EXPECT_EQ(finished.timing_info.request_start_microseconds, 40);
                        EXPECT_EQ(finished.timing_info.encoded_body_size, 8);
                        EXPECT(finished.network_error == Requests::NetworkError::IncompleteContent);
                    });
                seen_network_request_ids.append(network_activity.request_id.value());
            });
    }
    EXPECT_EQ(seen_ids, (Vector<u64> { 1, 2, 3, 4, 5, 6, 7 }));
    EXPECT_EQ(seen_request_body_events, (Vector<StringView> { "progress"sv, "end of body"sv, "read stream"sv, "sent from source"sv }));
    EXPECT_EQ(seen_network_request_ids, (Vector<u64> { 8, 9, 10, 11 }));
}
