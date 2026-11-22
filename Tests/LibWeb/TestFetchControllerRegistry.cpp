/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/EngineEvent.h>
#include <LibWeb/Fetch/Fetching/FetchEventSink.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>

using Web::Fetch::Fetching::FetchEventSink;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchControllerRegistry;
using Web::Fetch::Infrastructure::FetchControllerToken;

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

static NEVER_INLINE FetchControllerToken create_unreferenced_controller(FetchControllerRegistry& registry)
{
    return FetchController::create(registry)->token();
}

// Returns the token of a controller that only the given CORS preflight's controller references.
static NEVER_INLINE FetchControllerToken create_preflighted_fetch_controller(FetchControllerRegistry& registry, GC::Root<FetchController>& preflight_controller)
{
    auto controller = FetchController::create(registry);
    preflight_controller = GC::make_root(FetchController::create(registry));
    preflight_controller->set_preflighted_fetch_controller(controller);
    return controller->token();
}

static NEVER_INLINE bool is_registered(FetchControllerRegistry const& registry, FetchControllerToken token)
{
    return !!registry.find(token);
}

namespace {

struct EventSinkOfController {
    NonnullOwnPtr<FetchEventSink> event_sink;
    FetchControllerToken token;
};

}

static NEVER_INLINE EventSinkOfController create_event_sink_of_unreferenced_controller(FetchControllerRegistry& registry)
{
    auto controller = FetchController::create(registry);
    return { make<FetchEventSink>(controller), controller->token() };
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
}

TEST_CASE(each_registry_finds_only_its_own_controllers_by_unique_tokens)
{
    FetchControllerRegistry registry;
    FetchControllerRegistry other_registry;
    auto first_controller = GC::make_root(FetchController::create(registry));
    auto second_controller = GC::make_root(FetchController::create(registry));
    auto other_controller = GC::make_root(FetchController::create(other_registry));

    EXPECT(first_controller->token() != second_controller->token());
    EXPECT(first_controller->token() != other_controller->token());
    EXPECT(second_controller->token() != other_controller->token());

    EXPECT_EQ(registry.find(first_controller->token()).ptr(), first_controller.ptr());
    EXPECT_EQ(registry.find(second_controller->token()).ptr(), second_controller.ptr());
    EXPECT_EQ(other_registry.find(other_controller->token()).ptr(), other_controller.ptr());
}

TEST_CASE(a_collected_controller_is_not_found)
{
    FetchControllerRegistry registry;
    auto token = create_unreferenced_controller(registry);
    GC::scrub_stack_for_testing();

    test_heap().collect_garbage();

    EXPECT(!registry.find(token));
}

TEST_CASE(a_cors_preflights_controller_keeps_the_controller_of_its_fetch)
{
    FetchControllerRegistry registry;
    GC::Root<FetchController> preflight_controller;
    auto token = create_preflighted_fetch_controller(registry, preflight_controller);
    GC::scrub_stack_for_testing();

    test_heap().collect_garbage();
    EXPECT(is_registered(registry, token));

    preflight_controller = {};
    GC::scrub_stack_for_testing();
    test_heap().collect_garbage();
    EXPECT(!is_registered(registry, token));
}

TEST_CASE(a_controller_can_be_collected_after_its_registry_is_gone)
{
    {
        FetchControllerRegistry registry;
        (void)create_unreferenced_controller(registry);
    }

    test_heap().collect_garbage(GC::Heap::CollectionType::CollectEverything);
}

TEST_CASE(a_token_is_rejected_by_a_registry_that_did_not_issue_it)
{
    FetchControllerRegistry registry;
    FetchControllerRegistry other_registry;
    auto controller = GC::make_root(FetchController::create(registry));

    EXPECT_DEATH("Finding a token in another registry", (void)other_registry.find(controller->token()));
}

// The sink finds its controller through the registry for each event, and drops the event once either is gone.
TEST_CASE(an_event_for_a_collected_controller_or_a_registry_that_is_gone_is_dropped)
{
    FetchControllerRegistry registry;
    auto [event_sink, token] = create_event_sink_of_unreferenced_controller(registry);
    GC::scrub_stack_for_testing();
    test_heap().collect_garbage();
    EXPECT(!registry.find(token));
    event_sink->handle_event(Web::Fetch::Engine::Event::RequestBodyProgress { .length = 1 });

    OwnPtr<FetchEventSink> sink_of_registry_that_is_gone;
    GC::Root<FetchController> controller;
    {
        FetchControllerRegistry registry_that_is_gone;
        controller = GC::make_root(FetchController::create(registry_that_is_gone));
        sink_of_registry_that_is_gone = make<FetchEventSink>(*controller);
    }
    sink_of_registry_that_is_gone->handle_event(Web::Fetch::Engine::Event::RequestBodyProgress { .length = 1 });
}
