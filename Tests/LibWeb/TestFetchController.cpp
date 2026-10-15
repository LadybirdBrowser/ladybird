/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>

using Web::Fetch::Engine::WholeBodyTransport;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchControllerRegistry;

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
}

TEST_CASE(a_terminated_fetch_stops_a_body_transport_without_a_request)
{
    FetchControllerRegistry registry;
    auto controller = GC::make_root(FetchController::create(registry));
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    controller->set_body_transport(transport);

    controller->terminate();
    EXPECT(stopped);
}

TEST_CASE(stopping_the_body_transport_leaves_the_fetch_ongoing)
{
    FetchControllerRegistry registry;
    auto controller = GC::make_root(FetchController::create(registry));
    auto transport = WholeBodyTransport::create();
    bool stopped = false;
    transport->set_on_stop([&] { stopped = true; });
    controller->set_body_transport(transport);

    controller->stop_body_transport();
    EXPECT(stopped);
    EXPECT_EQ(controller->state(), FetchController::State::Ongoing);
}
