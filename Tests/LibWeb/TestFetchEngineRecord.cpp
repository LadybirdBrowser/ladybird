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
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Engine/WholeBodyRead.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>

using namespace Web::Fetch::Engine;

namespace {

struct FetchEnds : RefCounted<FetchEnds> {
    size_t count { 0 };
};

// Counts the ends of the fetch whose events it receives, and ignores every other event.
class EndCountingEventSink final : public EngineEventSink {
public:
    explicit EndCountingEventSink(RefPtr<FetchEnds> ends)
        : m_ends(move(ends))
    {
    }

    virtual void handle_event(EngineEvent event) override
    {
        if (event.has<Event::FetchEnded>() && m_ends)
            ++m_ends->count;
    }

private:
    RefPtr<FetchEnds> m_ends;
};

struct ReceivedEvents : RefCounted<ReceivedEvents> {
    Vector<StringView> events;
};

// Records the response deliveries and the end of the fetch whose events it receives, in the order they come.
class DeliveryAndEndRecordingEventSink final : public EngineEventSink {
public:
    explicit DeliveryAndEndRecordingEventSink(NonnullRefPtr<ReceivedEvents> received)
        : m_received(move(received))
    {
    }

    virtual void handle_event(EngineEvent event) override
    {
        if (event.has<Event::ResponseDelivery>())
            m_received->events.append("response delivery"sv);
        else if (event.has<Event::FetchEnded>())
            m_received->events.append("end of fetch"sv);
    }

private:
    NonnullRefPtr<ReceivedEvents> m_received;
};

struct MainFetchEndsAtDeliveries : RefCounted<MainFetchEndsAtDeliveries> {
    FetchEngineRecord* record { nullptr };
    Vector<bool> main_fetch_has_ended;
};

// Records, as each response delivery reaches it, whether main fetch of the record has ended.
class MainFetchEndRecordingEventSink final : public EngineEventSink {
public:
    explicit MainFetchEndRecordingEventSink(NonnullRefPtr<MainFetchEndsAtDeliveries> recorded)
        : m_recorded(move(recorded))
    {
    }

    virtual void handle_event(EngineEvent event) override
    {
        if (event.has<Event::ResponseDelivery>())
            m_recorded->main_fetch_has_ended.append(m_recorded->record->main_fetch_has_ended());
    }

private:
    NonnullRefPtr<MainFetchEndsAtDeliveries> m_recorded;
};

// Records misbehavior instead of dying, for calls that report it while they hold a reference to a record.
class MisbehaviorRecordingAgent final : public FetchEngineRecords::Agent {
public:
    explicit MisbehaviorRecordingAgent(Vector<ByteString>& misbehaviors)
        : m_misbehaviors(misbehaviors)
    {
    }

    virtual void misbehaved(StringView reason) override { m_misbehaviors.append(ByteString { reason }); }

private:
    Vector<ByteString>& m_misbehaviors;
};

class IgnoringProducerSink final : public FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
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

static NonnullRefPtr<FetchEngineRecord> start_fetch(FetchEngineRecords& records, u64 fetch_id, RefPtr<FetchEnds> ends = {})
{
    auto event_sink = make<EndCountingEventSink>(move(ends));
    return records.start(FetchId { fetch_id }, move(event_sink), NetworkActivityObservers::create()).release_nonnull();
}

static NonnullRefPtr<FetchEngineRecord> start_fetch_recording_events(FetchEngineRecords& records, u64 fetch_id, NonnullRefPtr<ReceivedEvents> received)
{
    auto event_sink = make<DeliveryAndEndRecordingEventSink>(move(received));
    return records.start(FetchId { fetch_id }, move(event_sink), NetworkActivityObservers::create()).release_nonnull();
}

// The delivery of a response without a body.
static Event::ResponseDelivery delivery()
{
    return {
        .response = Web::Fetch::Infrastructure::Response::create(),
        .body = NullBody {},
        .timing_seed = {},
        .redirect_continuation_id = {},
    };
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

// Opens the continuation of a manual redirect of the record's fetch, which stays open, and returns its id.
static ContinuationId open_continuation(FetchEngineRecord& record, NetworkTransportHandle* network_transport = nullptr)
{
    record.open_redirect_continuation(network_transport);
    return record.open_redirect_continuation_id().value();
}

// The record's fetch hands over a manual redirect whose response arrived through the transport, its body transport.
static ContinuationId hand_over_redirect(FetchEngineRecord& record, NetworkTransportWithGate& redirect)
{
    record.set_body_transport(*redirect.handle);
    auto continuation_id = open_continuation(record, redirect.handle.ptr());
    record.main_fetch_hands_over(delivery());
    return continuation_id;
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

TEST_CASE(a_followed_redirect_has_its_lease_released_and_its_request_stopped)
{
    Core::EventLoop event_loop;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto connected = connect_client();
    auto redirect = start_network_transport(*connected.client, Requests::RequestClient::TransferLease::Yes);
    auto continuation_id = hand_over_redirect(*record, redirect);
    EXPECT(has_lease(redirect));

    EXPECT_EQ(settled(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {}))).ptr(), record.ptr());
    EXPECT(!has_lease(redirect));
    EXPECT(redirect.teardowns->reasons == (Vector<Requests::Request::TeardownReason> { Requests::Request::TeardownReason::Stopped }));
}

// The load of the redirect's response can end as its body transport stops.
TEST_CASE(a_followed_redirect_keeps_its_fetch_through_the_end_of_its_load)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ends = make_ref_counted<FetchEnds>();
    auto record = start_fetch(records, 1, ends);
    auto load = record->start_load();
    auto transport = WholeBodyTransport::create();
    transport->set_on_stop([load] { load->end(); });
    record->set_body_transport(transport);
    auto continuation_id = open_continuation(*record);
    record->main_fetch_hands_over(delivery());

    EXPECT_EQ(settled(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {}))).ptr(), record.ptr());
    EXPECT(load->has_ended());
    EXPECT_EQ(ends->count, 0u);
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
    record->open_redirect_continuation(redirect.handle.ptr());
    EXPECT(!has_lease(redirect));
    EXPECT(!record->open_redirect_continuation_id().has_value());
}

TEST_CASE(an_answer_for_a_settled_redirect_or_an_ended_fetch_does_nothing)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ended_continuation_id = open_continuation(*start_fetch(records, 1));
    EXPECT(was_dropped(records.resolve_redirect(answer(1, ended_continuation_id, Command::ResolveRedirect::Follow {}))));

    auto record = start_fetch(records, 2);
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    record->set_body_transport(transport);
    auto continuation_id = open_continuation(*record);
    record->main_fetch_hands_over(delivery());
    EXPECT_EQ(settled(records.resolve_redirect(answer(2, continuation_id, Command::ResolveRedirect::AcceptAsFinal {}))).ptr(), record.ptr());

    EXPECT(was_dropped(records.resolve_redirect(answer(2, continuation_id, Command::ResolveRedirect::Abandon {}))));
    EXPECT(was_dropped(records.resolve_redirect(answer(2, continuation_id, Command::ResolveRedirect::Follow {}))));
    EXPECT(!stopped);
}

TEST_CASE(an_answer_for_a_redirect_the_engine_never_offered_dies)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    auto continuation_id = open_continuation(*record);

    auto above_every_offered = answer(1, ContinuationId { continuation_id.value() + 1 }, Command::ResolveRedirect::Follow {});
    EXPECT_DEATH("Answering a redirect with a continuation id above every one the engine offered", (void)records.resolve_redirect(above_every_offered));

    auto of_a_fetch_not_started = answer(2, continuation_id, Command::ResolveRedirect::Follow {});
    EXPECT_DEATH("Answering a redirect of a fetch with an id above that of the last fetch started", (void)records.resolve_redirect(of_a_fetch_not_started));
}

TEST_CASE(an_answer_to_a_redirect_main_fetch_has_not_handed_over_is_misbehavior)
{
    Vector<ByteString> misbehaviors;
    FetchEngineRecords records { make<MisbehaviorRecordingAgent>(misbehaviors) };
    auto record = start_fetch(records, 1);
    auto continuation_id = open_continuation(*record);

    EXPECT(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {})).has<FetchEngineRecords::RedirectAnswerMisbehaved>());
    EXPECT(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Abandon {})).has<FetchEngineRecords::RedirectAnswerMisbehaved>());
    EXPECT_EQ(misbehaviors, (Vector<ByteString> { "The agent answered a redirect before it was handed over", "The agent answered a redirect before it was handed over" }));
}

TEST_CASE(a_fetch_ends_once_main_fetch_has_handed_over_and_its_loads_have_ended)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ends = make_ref_counted<FetchEnds>();
    auto record = start_fetch(records, 1, ends);
    auto first_load = record->start_load();
    auto second_load = record->start_load();

    record->main_fetch_hands_over(delivery());
    first_load->end();
    EXPECT_EQ(ends->count, 0u);

    second_load->end();
    EXPECT_EQ(ends->count, 1u);

    // Nothing sends the end a second time.
    second_load->end();
    record->main_fetch_cannot_hand_over();
    EXPECT_EQ(ends->count, 1u);
}

TEST_CASE(a_fetch_whose_loads_have_ended_ends_at_its_handover_after_the_delivery)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto received = make_ref_counted<ReceivedEvents>();
    auto record = start_fetch_recording_events(records, 1, received);
    record->start_load()->end();
    EXPECT(received->events.is_empty());

    record->main_fetch_hands_over(delivery());
    EXPECT_EQ(received->events, (Vector<StringView> { "response delivery"sv, "end of fetch"sv }));
}

TEST_CASE(main_fetch_has_ended_once_its_delivery_reaches_the_agent)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto recorded = make_ref_counted<MainFetchEndsAtDeliveries>();
    auto record = records.start(FetchId { 1 }, make<MainFetchEndRecordingEventSink>(recorded), NetworkActivityObservers::create()).release_nonnull();
    recorded->record = record.ptr();

    record->main_fetch_hands_over(delivery());
    EXPECT_EQ(recorded->main_fetch_has_ended, (Vector<bool> { true }));
}

TEST_CASE(a_fetch_ends_once_the_redirect_it_handed_over_is_settled)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto accepted_ends = make_ref_counted<FetchEnds>();
    auto abandoned_ends = make_ref_counted<FetchEnds>();
    auto stopped_ends = make_ref_counted<FetchEnds>();
    auto accepted = start_fetch(records, 1, accepted_ends);
    auto abandoned = start_fetch(records, 2, abandoned_ends);
    auto stopped = start_fetch(records, 3, stopped_ends);
    Vector<ContinuationId> continuation_ids;
    for (auto const& record : { accepted, abandoned, stopped }) {
        continuation_ids.append(open_continuation(*record));
        record->main_fetch_hands_over(delivery());
    }
    EXPECT_EQ(accepted_ends->count + abandoned_ends->count + stopped_ends->count, 0u);

    (void)records.resolve_redirect(answer(1, continuation_ids[0], Command::ResolveRedirect::AcceptAsFinal {}));
    (void)records.resolve_redirect(answer(2, continuation_ids[1], Command::ResolveRedirect::Abandon {}));
    records.stop(FetchId { 3 });
    EXPECT_EQ(accepted_ends->count, 1u);
    EXPECT_EQ(abandoned_ends->count, 1u);
    EXPECT_EQ(stopped_ends->count, 1u);
}

TEST_CASE(a_followed_redirect_keeps_its_fetch_until_the_next_hop_ends)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ends = make_ref_counted<FetchEnds>();
    auto record = start_fetch(records, 1, ends);
    auto continuation_id = open_continuation(*record);
    record->main_fetch_hands_over(delivery());

    EXPECT_EQ(settled(records.resolve_redirect(answer(1, continuation_id, Command::ResolveRedirect::Follow {}))).ptr(), record.ptr());
    auto next_hop_load = record->start_load();
    record->main_fetch_hands_over(delivery());
    EXPECT_EQ(ends->count, 0u);

    next_hop_load->end();
    EXPECT_EQ(ends->count, 1u);
}

TEST_CASE(main_fetch_that_can_no_longer_hand_over_ends_its_fetch)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto ends = make_ref_counted<FetchEnds>();
    auto record = start_fetch(records, 1, ends);
    auto load = record->start_load();
    record->main_fetch_cannot_hand_over();
    EXPECT_EQ(ends->count, 0u);

    load->end();
    EXPECT_EQ(ends->count, 1u);
}

// Main fetch reads a body to its end before it hands the response over, as it does to check the body's integrity. A read
// that cannot reach the end of the body hands a network error over in its place.
TEST_CASE(a_fetch_whose_body_main_fetch_reads_ends_once_the_read_hands_over)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto make_channel = [] { return FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink)); };
    Vector<StringView> outcomes;
    auto read_and_hand_over = [&outcomes](NonnullRefPtr<FetchEngineRecord> const& record, NonnullRefPtr<FetchByteChannel> const& channel) {
        read_whole_body(Web::Fetch::Infrastructure::ResponseBody { channel, {} }, [record, &outcomes](WholeBodyReadResult result) {
            outcomes.append(result.has<WholeBodyReadFailure>() ? "failure"sv : "bytes"sv);
            record->main_fetch_hands_over(delivery());
        });
    };

    // A body cancelled or transferred while its load runs.
    auto cancelled_ends = make_ref_counted<FetchEnds>();
    auto transferred_ends = make_ref_counted<FetchEnds>();
    auto cancelled = start_fetch(records, 1, cancelled_ends);
    auto transferred = start_fetch(records, 2, transferred_ends);
    auto cancelled_load = cancelled->start_load();
    auto transferred_load = transferred->start_load();
    auto cancelled_channel = make_channel();
    auto transferred_channel = make_channel();
    read_and_hand_over(cancelled, cancelled_channel);
    read_and_hand_over(transferred, transferred_channel);
    EXPECT(outcomes.is_empty());
    (void)cancelled_channel->cancel();
    (void)transferred_channel->transfer_out();
    EXPECT_EQ(outcomes, (Vector<StringView> { "failure"sv, "failure"sv }));
    EXPECT_EQ(cancelled_ends->count + transferred_ends->count, 0u);
    cancelled_load->end();
    transferred_load->end();
    EXPECT_EQ(cancelled_ends->count, 1u);
    EXPECT_EQ(transferred_ends->count, 1u);

    // A body cancelled before main fetch reads it, after its load has ended.
    auto cancelled_before_ends = make_ref_counted<FetchEnds>();
    auto cancelled_before = start_fetch(records, 3, cancelled_before_ends);
    cancelled_before->start_load()->end();
    EXPECT_EQ(cancelled_before_ends->count, 0u);
    auto cancelled_before_channel = make_channel();
    (void)cancelled_before_channel->cancel();
    read_and_hand_over(cancelled_before, cancelled_before_channel);
    EXPECT_EQ(outcomes.last(), "failure"sv);
    EXPECT_EQ(cancelled_before_ends->count, 1u);

    // A body that closes is read to its end.
    auto closed_ends = make_ref_counted<FetchEnds>();
    auto closed = start_fetch(records, 4, closed_ends);
    auto closed_load = closed->start_load();
    auto closed_channel = make_channel();
    read_and_hand_over(closed, closed_channel);
    (void)closed_channel->close();
    EXPECT_EQ(outcomes.last(), "bytes"sv);
    EXPECT_EQ(closed_ends->count, 0u);
    closed_load->end();
    EXPECT_EQ(closed_ends->count, 1u);
}

TEST_CASE(a_stop_during_the_read_of_the_body_main_fetch_reads_hands_its_failure_over)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto received = make_ref_counted<ReceivedEvents>();
    auto record = start_fetch_recording_events(records, 1, received);
    auto load = record->start_load();
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink));
    auto transport = WholeBodyTransport::create();
    transport->set_on_stop([channel] { (void)channel->cancel(); });
    record->set_body_transport(transport);
    EXPECT_EQ(channel->write(MUST(Core::ImmutableBytes::copy("the start"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);

    Vector<StringView> outcomes;
    read_whole_body(Web::Fetch::Infrastructure::ResponseBody { channel, {} }, [record, &outcomes](WholeBodyReadResult result) {
        outcomes.append(result.has<WholeBodyReadFailure>() ? "failure"sv : "bytes"sv);
        record->main_fetch_hands_over(delivery());
    });
    EXPECT(outcomes.is_empty());

    records.stop(FetchId { 1 });
    EXPECT_EQ(outcomes, (Vector<StringView> { "failure"sv }));
    EXPECT_EQ(received->events, (Vector<StringView> { "response delivery"sv }));

    load->end();
    EXPECT_EQ(received->events, (Vector<StringView> { "response delivery"sv, "end of fetch"sv }));
}

TEST_CASE(a_load_after_the_end_of_its_fetch_dies)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    record->main_fetch_hands_over(delivery());

    EXPECT_DEATH("Starting a load of a fetch that has ended", (void)record->start_load());
}

TEST_CASE(a_handover_after_main_fetch_has_ended_delivers_nothing)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto received = make_ref_counted<ReceivedEvents>();
    auto record = start_fetch_recording_events(records, 1, received);
    record->main_fetch_cannot_hand_over();
    EXPECT(record->main_fetch_has_ended());
    EXPECT_EQ(received->events, (Vector<StringView> { "end of fetch"sv }));

    record->main_fetch_hands_over(delivery());
    EXPECT_EQ(received->events, (Vector<StringView> { "end of fetch"sv }));
}

TEST_CASE(an_abandoned_preload_wait_ends_the_fetch_whose_main_fetch_waits)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto waiting_ends = make_ref_counted<FetchEnds>();
    auto waiting = start_fetch(records, 1, waiting_ends);
    waiting->main_fetch_waits_for_preload();
    records.abandon_preload_wait(FetchId { 1 });
    EXPECT_EQ(waiting_ends->count, 1u);

    // The fetch has ended, and a later abandonment changes nothing.
    records.abandon_preload_wait(FetchId { 1 });
    EXPECT_EQ(waiting_ends->count, 1u);

    // A wait the agent resolved ends as any other main fetch does.
    auto resolved_ends = make_ref_counted<FetchEnds>();
    auto resolved = start_fetch(records, 2, resolved_ends);
    resolved->main_fetch_waits_for_preload();
    resolved->main_fetch_stops_waiting_for_preload();
    EXPECT_EQ(resolved_ends->count, 0u);
    resolved->main_fetch_hands_over(delivery());
    EXPECT_EQ(resolved_ends->count, 1u);
    records.abandon_preload_wait(FetchId { 2 });
    EXPECT_EQ(resolved_ends->count, 1u);
}

TEST_CASE(an_abandoned_preload_wait_of_a_fetch_that_waits_for_none_is_misbehavior)
{
    Vector<ByteString> misbehaviors;
    FetchEngineRecords records { make<MisbehaviorRecordingAgent>(misbehaviors) };
    auto record = start_fetch(records, 1);
    records.abandon_preload_wait(FetchId { 1 });

    record->main_fetch_waits_for_preload();
    record->main_fetch_stops_waiting_for_preload();
    records.abandon_preload_wait(FetchId { 1 });
    records.abandon_preload_wait(FetchId { 2 });

    EXPECT(!record->has_ended());
    EXPECT_EQ(misbehaviors, (Vector<ByteString> {
                                "The agent abandoned a preload wait of a fetch that waits for no preload",
                                "The agent abandoned a preload wait of a fetch that waits for no preload",
                                "The agent abandoned a preload wait of a fetch it has not started",
                            }));
}

TEST_CASE(a_handover_while_main_fetch_waits_for_a_preload_dies)
{
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto record = start_fetch(records, 1);
    record->main_fetch_waits_for_preload();

    EXPECT_DEATH("Handing a response over while main fetch waits for a preload", record->main_fetch_hands_over(delivery()));
}
