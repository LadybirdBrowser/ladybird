/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>

using namespace Web::Fetch::Engine;

namespace {

class IgnoringEventSink final : public EngineEventSink {
public:
    virtual void handle_event(EngineEvent) override { }
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

    records.stop_body_transport(FetchId { 1 });
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

    records.stop_body_transport(FetchId { 1 });
    EXPECT(!ended_stopped);
    EXPECT(!stopped);
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

    EXPECT_DEATH("Stopping a fetch with an id above that of the last fetch started", records.stop_body_transport(FetchId { 2 }));
}
