/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/WindowCloseCoordinator.h>

TEST_CASE(preflights_run_sequentially)
{
    Ladybird::WindowCloseCoordinator coordinator;
    Vector<size_t> started;
    Vector<Function<void(bool)>> completions;
    Vector<Ladybird::WindowCloseCoordinator::Preflight> preflights;

    for (size_t index = 0; index < 3; ++index) {
        preflights.append([&, index](auto on_complete) {
            started.append(index);
            completions.append(move(on_complete));
        });
    }

    bool approved = false;
    bool canceled = false;
    EXPECT(coordinator.start(move(preflights), [&] { approved = true; }, [&] { canceled = true; }));
    EXPECT(coordinator.is_running());
    EXPECT_EQ(started, Vector<size_t> { 0 });

    completions[0](true);
    EXPECT_EQ(started, (Vector<size_t> { 0, 1 }));
    completions[1](true);
    EXPECT_EQ(started, (Vector<size_t> { 0, 1, 2 }));
    completions[2](true);

    EXPECT(approved);
    EXPECT(!canceled);
    EXPECT(!coordinator.is_running());
}

TEST_CASE(cancellation_stops_the_sequence)
{
    Ladybird::WindowCloseCoordinator coordinator;
    Vector<Function<void(bool)>> completions;
    Vector<Ladybird::WindowCloseCoordinator::Preflight> preflights;
    preflights.append([&](auto on_complete) { completions.append(move(on_complete)); });
    preflights.append([&](auto on_complete) { completions.append(move(on_complete)); });
    preflights.append([&](auto on_complete) { completions.append(move(on_complete)); });

    bool approved = false;
    bool canceled = false;
    EXPECT(coordinator.start(move(preflights), [&] { approved = true; }, [&] { canceled = true; }));
    completions[0](true);
    completions[1](false);

    EXPECT(!approved);
    EXPECT(canceled);
    EXPECT_EQ(completions.size(), 2u);
    EXPECT(!coordinator.is_running());
}

TEST_CASE(repeated_start_and_late_completions_are_ignored)
{
    Ladybird::WindowCloseCoordinator coordinator;
    Function<void(bool)> completion;
    Vector<Ladybird::WindowCloseCoordinator::Preflight> preflights;
    preflights.append([&](auto on_complete) { completion = move(on_complete); });

    size_t canceled_count = 0;
    EXPECT(coordinator.start(move(preflights), [] {}, [&] { ++canceled_count; }));
    EXPECT(!coordinator.start({}, [] { }, [] { }));
    coordinator.cancel();
    EXPECT_EQ(canceled_count, 1u);

    completion(true);
    EXPECT_EQ(canceled_count, 1u);
    EXPECT(!coordinator.is_running());
}

TEST_CASE(an_empty_preflight_is_immediately_approved)
{
    Ladybird::WindowCloseCoordinator coordinator;
    bool approved = false;
    EXPECT(coordinator.start({}, [&] { approved = true; }, [] {}));
    EXPECT(approved);
    EXPECT(!coordinator.is_running());
}
