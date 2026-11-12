/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Loader/LoaderConfig.h>

using Web::Fetch::Engine::FetchEngineRecords;
using Web::Fetch::Engine::WholeBodyTransport;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchControllerRegistry;
using Web::Fetch::Infrastructure::FetchParams;

namespace {

class IgnoringEventSink final : public Web::Fetch::Engine::EngineEventSink {
public:
    virtual void handle_event(Web::Fetch::Engine::EngineEvent) override { }
};

}

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
}

static NonnullRefPtr<FetchParams> create_fetch_params(FetchController& controller)
{
    auto algorithms = FetchAlgorithms::create(FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard });
    auto request = Web::Fetch::Infrastructure::Request::create();
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    return FetchParams::create(controller, move(request), algorithms, move(timing_info), Web::current_loader_config());
}

static NonnullRefPtr<Web::Fetch::Engine::FetchEngineRecord> start_fetch(FetchEngineRecords& records)
{
    auto event_sink = make<IgnoringEventSink>();
    return records.start(Web::Fetch::Engine::FetchId { 1 }, move(event_sink), Web::Fetch::Engine::NetworkActivityObservers::create()).release_nonnull();
}

TEST_CASE(a_controller_that_started_no_fetch_can_be_terminated)
{
    FetchControllerRegistry registry;
    auto controller = GC::make_root(FetchController::create(registry));

    controller->terminate();
    EXPECT_EQ(controller->state(), FetchController::State::Terminated);
}

TEST_CASE(stopping_the_body_transport_leaves_the_fetch_ongoing)
{
    FetchControllerRegistry registry;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto controller = GC::make_root(FetchController::create(registry));
    auto fetch_params = create_fetch_params(*controller);
    fetch_params->set_engine_record(start_fetch(records));
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    fetch_params->set_body_transport(transport);

    fetch_params->stop_body_transport();
    EXPECT(stopped);
    EXPECT_EQ(controller->state(), FetchController::State::Ongoing);
}

TEST_CASE(a_cors_preflight_stops_its_own_body_transport_and_not_that_of_its_fetch)
{
    FetchControllerRegistry registry;
    FetchEngineRecords records { make<FetchEngineRecords::Agent>() };
    auto controller = GC::make_root(FetchController::create(registry));
    auto fetch_params = create_fetch_params(*controller);
    fetch_params->set_engine_record(start_fetch(records));
    auto preflight_controller = GC::make_root(FetchController::create(registry));
    auto preflight_fetch_params = create_fetch_params(*preflight_controller);
    preflight_fetch_params->set_engine_record(*fetch_params->engine_record());
    preflight_fetch_params->set_is_cors_preflight();

    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    fetch_params->set_body_transport(transport);
    auto preflight_transport = WholeBodyTransport::create();
    bool preflight_stopped = false;
    preflight_transport->set_on_stop([&] { preflight_stopped = true; });
    preflight_fetch_params->set_body_transport(preflight_transport);

    preflight_fetch_params->stop_body_transport();
    EXPECT(preflight_stopped);
    EXPECT(!stopped);

    fetch_params->stop_body_transport();
    EXPECT(stopped);
}
