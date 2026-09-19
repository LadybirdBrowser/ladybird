/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/AudioOutputMonitor.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

TEST_CASE(reports_only_non_silent_enabled_output)
{
    auto& event_loop = never_destroyed_event_loop();
    auto monitor = Audio::AudioOutputMonitor::create(event_loop);
    Vector<bool> states;
    monitor->set_output_state_change_handler([&](bool is_non_silent) {
        states.append(is_non_silent);
    });

    monitor->update(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(states.is_empty());

    monitor->set_enabled(true);
    monitor->update(false);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(states.is_empty());

    monitor->update(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(states, (Vector<bool> { true }));

    monitor->update(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(states, (Vector<bool> { true }));

    monitor->update(false);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(states, (Vector<bool> { true, false }));
}

TEST_CASE(muting_and_disabling_publish_silence)
{
    auto& event_loop = never_destroyed_event_loop();
    auto monitor = Audio::AudioOutputMonitor::create(event_loop);
    Vector<bool> states;
    monitor->set_output_state_change_handler([&](bool is_non_silent) {
        states.append(is_non_silent);
    });

    monitor->set_enabled(true);
    monitor->update(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    monitor->set_muted(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    monitor->update(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(states, (Vector<bool> { true, false }));

    monitor->set_muted(false);
    monitor->update(true);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(states, (Vector<bool> { true, false, true }));

    monitor->set_enabled(false);
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(states, (Vector<bool> { true, false, true, false }));
}
