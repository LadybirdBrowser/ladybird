/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>

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

    test_heap().collect_garbage(GC::Heap::CollectionType::CollectEverything);

    EXPECT(!registry.find(token));
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
