/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>

using namespace Web::Fetch::Engine;

namespace {

class IgnoringEventSink final : public EngineEventSink {
public:
    virtual void handle_event(EngineEvent) override { }
};

// A RequestServer client whose RequestServer end of the connection is held, and never read.
struct ConnectedClient {
    NonnullRefPtr<Requests::RequestClient> client;
    IPC::TransportHandle request_server;
};

struct TeardownReasons : RefCounted<TeardownReasons> {
    Vector<Requests::Request::TeardownReason> reasons;
};

// The transport of a request, and a delivery gate that holds a body for its navigation, with no transport attached.
struct NetworkTransportWithGate {
    NonnullRefPtr<NetworkTransportHandle> handle;
    NonnullRefPtr<TeardownReasons> teardowns;
    NonnullRefPtr<FetchBodyDeliveryGate> delivery_gate;
};

}

static NonnullRefPtr<FetchEngineRecord> start_fetch(FetchEngineRecords& records, u64 fetch_id)
{
    auto event_sink = make<IgnoringEventSink>();
    return records.start(FetchId { fetch_id }, move(event_sink), NetworkActivityObservers::create()).release_nonnull();
}

TEST_CASE(a_stop_of_a_fetch_stops_its_body_transport_and_leaves_that_of_its_cors_preflight)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    record->set_body_transport(transport);
    auto preflight_transport = WholeBodyTransport::create();
    bool preflight_stopped = false;
    preflight_transport->set_on_stop([&] { preflight_stopped = true; });
    record->set_cors_preflight_body_transport(preflight_transport);

    records.stop(FetchId { 1 });
    EXPECT(stopped);
    EXPECT(!preflight_stopped);

    record->stop_cors_preflight_body_transport();
    EXPECT(preflight_stopped);
}

TEST_CASE(a_stop_of_a_fetch_whose_record_is_gone_stops_nothing)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ended_transport = WholeBodyTransport::create();
    bool ended_stopped = false;
    ended_transport->set_on_stop([&] { ended_stopped = true; });
    start_fetch(records, 1)->set_body_transport(ended_transport);
    auto record = start_fetch(records, 2);
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    record->set_body_transport(transport);

    records.stop(FetchId { 1 });
    EXPECT(!ended_stopped);
    EXPECT(!stopped);
}

static ConnectedClient connect_client()
{
    auto pair = MUST(IPC::Transport::create_paired());
    auto client = make_ref_counted<Requests::RequestClient>(move(pair.local), 1);
    return { move(client), move(pair.remote_handle) };
}

static NetworkTransportWithGate start_network_transport(Requests::RequestClient& client, Requests::RequestClient::TransferLease transfer_lease = Requests::RequestClient::TransferLease::No)
{
    auto url = URL::Parser::basic_parse("https://example.com/"sv);
    VERIFY(url.has_value());
    auto request = client.start_request("GET"sv, *url, HTTP::Cookie::SameSiteContext {}, {}, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, transfer_lease);
    VERIFY(request);

    auto handle = NetworkTransportHandle::create(request.release_nonnull());
    auto teardowns = make_ref_counted<TeardownReasons>();
    handle->set_on_teardown([teardowns](Requests::Request::TeardownReason reason) { teardowns->reasons.append(reason); });
    auto delivery_gate = FetchBodyDeliveryGate::create(FetchBodyDeliveryGate::NavigationHold::Held);
    delivery_gate->grant_credit(100);
    return { move(handle), move(teardowns), move(delivery_gate) };
}

static Command::TransportControl control_of(NetworkTransportHandle const& handle, Command::TransportControl::Operation operation)
{
    return { .request_id = handle.request_id(), .transport_epoch = handle.transport_epoch(), .operation = move(operation) };
}

TEST_CASE(a_transport_control_reaches_only_the_transport_with_its_request_id_and_epoch)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };

    // Request ids start again for each client, as for a client that replaces one whose RequestServer died.
    auto first_client = connect_client();
    auto second_client = connect_client();
    auto first = start_network_transport(*first_client.client);
    auto second = start_network_transport(*second_client.client);
    EXPECT_EQ(first.handle->request_id(), second.handle->request_id());
    records.add_network_transport(*first.handle, *first.delivery_gate);
    records.add_network_transport(*second.handle, *second.delivery_gate);

    records.control_network_transport(control_of(*second.handle, Command::TransportControl::ReleaseNavigationHoldUpTo { .byte_count = 10 }));
    EXPECT(second.delivery_gate->did_deliver(10) == FetchBodyDeliveryGate::DeliveryResult::Accepted);
    EXPECT(second.delivery_gate->did_deliver(1) == FetchBodyDeliveryGate::DeliveryResult::Rejected);
    EXPECT(first.delivery_gate->did_deliver(1) == FetchBodyDeliveryGate::DeliveryResult::Rejected);

    records.control_network_transport(control_of(*first.handle, Command::TransportControl::ReleaseNavigationHold {}));
    EXPECT(first.delivery_gate->did_deliver(100) == FetchBodyDeliveryGate::DeliveryResult::Accepted);

    records.control_network_transport(control_of(*first.handle, Command::TransportControl::StopTransport {}));
    EXPECT(first.teardowns->reasons == (Vector<Requests::Request::TeardownReason> { Requests::Request::TeardownReason::Stopped }));
    EXPECT(second.teardowns->reasons.is_empty());
}

TEST_CASE(a_transport_control_of_a_network_transport_that_is_gone_does_nothing)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };

    auto connected = connect_client();
    auto delivery_gate = FetchBodyDeliveryGate::create(FetchBodyDeliveryGate::NavigationHold::Held);
    delivery_gate->grant_credit(100);
    Optional<Command::TransportControl> release_hold;
    {
        auto transport = start_network_transport(*connected.client);
        records.add_network_transport(*transport.handle, *delivery_gate);
        release_hold = control_of(*transport.handle, Command::TransportControl::ReleaseNavigationHold {});
    }

    // Adding enough of another client's transports forgets the one that is gone.
    auto other_client = connect_client();
    Vector<NetworkTransportWithGate> others;
    for (size_t i = 0; i < FetchEngineRecords::smallest_network_transport_count_to_sweep_at; ++i) {
        auto other = start_network_transport(*other_client.client);
        records.add_network_transport(*other.handle, *other.delivery_gate);
        others.append(move(other));
    }

    records.control_network_transport(*release_hold);
    EXPECT(delivery_gate->did_deliver(1) == FetchBodyDeliveryGate::DeliveryResult::Rejected);
}

TEST_CASE(a_transport_control_of_a_request_the_engine_never_had_dies)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };

    auto connected = connect_client();
    auto transport = start_network_transport(*connected.client);
    records.add_network_transport(*transport.handle, *transport.delivery_gate);

    auto above = control_of(*transport.handle, Command::TransportControl::StopTransport {});
    above.request_id = RequestId { above.request_id.value() + 1 };
    EXPECT_DEATH("Controlling a request with an id above those of its client's requests the engine had", records.control_network_transport(above));

    auto other_client = connect_client();
    auto of_other_client = control_of(*transport.handle, Command::TransportControl::StopTransport {});
    of_other_client.transport_epoch = TransportEpoch { other_client.client->transport_epoch() };
    EXPECT_DEATH("Controlling a request of a client the engine had none of", records.control_network_transport(of_other_client));
}

TEST_CASE(a_start_of_a_fetch_out_of_fetch_id_order_dies)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 2);

    EXPECT_DEATH("Starting a fetch with the id of the last fetch started", (void)start_fetch(records, 2));
    EXPECT_DEATH("Starting a fetch with an id below that of the last fetch started", (void)start_fetch(records, 1));
}

TEST_CASE(a_stop_of_a_fetch_the_engine_has_not_started_dies)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);

    EXPECT_DEATH("Stopping a fetch with an id above that of the last fetch started", records.stop(FetchId { 2 }));
}

static Command::ResolveRedirect answer(u64 fetch_id, ContinuationId continuation_id, Command::ResolveRedirect::Resolution resolution)
{
    return { .fetch_id = FetchId { fetch_id }, .continuation_id = continuation_id, .resolution = move(resolution) };
}

// The record of the fetch whose continuation the answer settled, or null for any other outcome.
static RefPtr<FetchEngineRecord> settled(FetchEngineRecords::RedirectAnswerOutcome outcome)
{
    if (auto* record = outcome.get_pointer<NonnullRefPtr<FetchEngineRecord>>())
        return *record;
    return {};
}

static bool was_dropped(FetchEngineRecords::RedirectAnswerOutcome const& outcome)
{
    return outcome.has<FetchEngineRecords::RedirectAnswerDropped>();
}

static bool has_lease(NetworkTransportWithGate const& transport)
{
    return transport.handle->transfer_lease().has_value();
}

// The record's fetch hands over a manual redirect whose response arrived through the transport, its body transport.
static ContinuationId hand_over_redirect(FetchEngineRecord& record, NetworkTransportWithGate& redirect)
{
    record.set_body_transport(*redirect.handle);
    return record.open_redirect_continuation(redirect.handle.ptr());
}

TEST_CASE(an_abandoned_redirect_has_its_lease_released_and_its_request_stopped)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto connected = connect_client();
    auto redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    auto continuation_id = hand_over_redirect(*record, redirect);
    EXPECT(has_lease(redirect));

    EXPECT_EQ(settled(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Abandon {}))).ptr(), record.ptr());
    EXPECT(!has_lease(redirect));
    EXPECT(redirect.teardowns->reasons == (Vector<Requests::Request::TeardownReason> { Requests::Request::TeardownReason::Stopped }));
}

TEST_CASE(a_redirect_accepted_as_final_keeps_its_request)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto connected = connect_client();
    auto redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    auto continuation_id = hand_over_redirect(*record, redirect);

    EXPECT_EQ(settled(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::AcceptAsFinal {}))).ptr(), record.ptr());
    EXPECT(has_lease(redirect));
    EXPECT(redirect.teardowns->reasons.is_empty());

    // A stop of the fetch then stops the request as that of any other response it handed over.
    records.stop(FetchId { 1 });
    EXPECT(has_lease(redirect));
    EXPECT(redirect.teardowns->reasons == (Vector<Requests::Request::TeardownReason> { Requests::Request::TeardownReason::Stopped }));
}

TEST_CASE(a_followed_redirect_gives_its_record_to_the_next_hop)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto connected = connect_client();
    auto redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    auto continuation_id = hand_over_redirect(*record, redirect);

    EXPECT_EQ(settled(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {}))).ptr(), record.ptr());

    // The next hop hands over a redirect of its own.
    auto next_redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    auto next_continuation_id = hand_over_redirect(*record, next_redirect);
    EXPECT_NE(next_continuation_id, continuation_id);
    EXPECT_EQ(settled(records.resolve_redirect(answer(1, next_continuation_id, Command::ResolveRedirect::Follow {}))).ptr(), record.ptr());
}

TEST_CASE(a_stop_of_a_fetch_abandons_the_redirect_it_waits_on)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto connected = connect_client();
    auto redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    auto continuation_id = hand_over_redirect(*record, redirect);

    records.stop(FetchId { 1 });
    EXPECT(!has_lease(redirect));
    EXPECT(redirect.teardowns->reasons == (Vector<Requests::Request::TeardownReason> { Requests::Request::TeardownReason::Stopped }));

    // The agent's task that follows the redirect before it heard of the stop gets no next hop.
    EXPECT(was_dropped(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {}))));
}

TEST_CASE(a_redirect_handed_over_after_a_stop_of_its_fetch_is_abandoned_at_once)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto connected = connect_client();
    auto redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    record->set_body_transport(*redirect.handle);

    // The pipeline hands the redirect over once the stop has stopped its request.
    records.stop(FetchId { 1 });
    EXPECT(has_lease(redirect));
    auto continuation_id = record->open_redirect_continuation(redirect.handle.ptr());
    EXPECT(!has_lease(redirect));
    EXPECT(was_dropped(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {}))));
}

TEST_CASE(an_answer_for_a_settled_redirect_or_an_ended_fetch_does_nothing)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ended_continuation_id = start_fetch(records, 1)->open_redirect_continuation(nullptr);
    EXPECT(was_dropped(records.resolve_redirect(answer(1, ended_continuation_id, Command::ResolveRedirect::Follow {}))));

    auto record = start_fetch(records, 2);
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    record->set_body_transport(transport);
    auto continuation_id = record->open_redirect_continuation(nullptr);
    EXPECT_EQ(settled(records.resolve_redirect(answer(2, continuation_id, Command::ResolveRedirect::AcceptAsFinal {}))).ptr(), record.ptr());

    EXPECT(was_dropped(records.resolve_redirect(answer(2, continuation_id, Command::ResolveRedirect::Abandon {}))));
    EXPECT(was_dropped(records.resolve_redirect(answer(2, continuation_id, Command::ResolveRedirect::Follow {}))));
    EXPECT(!stopped);
}

TEST_CASE(an_answer_for_a_redirect_the_engine_never_offered_dies)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto continuation_id = record->open_redirect_continuation(nullptr);

    auto above_every_offered = answer(1, ContinuationId { continuation_id.value() + 1 }, Command::ResolveRedirect::Follow {});
    EXPECT_DEATH("Answering a redirect with a continuation id above every one the engine offered", (void)records.resolve_redirect(above_every_offered));

    auto of_a_fetch_not_started = answer(2, continuation_id, Command::ResolveRedirect::Follow {});
    EXPECT_DEATH("Answering a redirect of a fetch with an id above that of the last fetch started", (void)records.resolve_redirect(of_a_fetch_not_started));
}
