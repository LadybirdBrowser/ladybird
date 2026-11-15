/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibTest/TestCase.h>
#include <LibWebView/NowPlayingInfo.h>

using Web::Bindings::MediaSessionAction;
using Web::Bindings::MediaSessionPlaybackState;
using WebView::now_playing_info_for;
using WebView::NowPlayingPlaybackStatus;
using WebView::NowPlayingState;

static constexpr MonotonicTime now = MonotonicTime {} + AK::Duration::from_seconds(10);

static NowPlayingState playing_state()
{
    NowPlayingState state;
    state.title = "Title"_string;
    state.artist = "Artist"_string;
    state.album = "Album"_string;
    state.playback_state = MediaSessionPlaybackState::Playing;
    state.position = Web::MediaSession::MediaSessionPositionState { 20, 1.5, 4, now - AK::Duration::from_seconds(2) };
    state.supported_actions = { MediaSessionAction::Play, MediaSessionAction::Pause, MediaSessionAction::Stop, MediaSessionAction::Seekto };
    return state;
}

TEST_CASE(now_playing_without_a_session)
{
    auto info = now_playing_info_for({}, now);
    EXPECT(info.playback_status == NowPlayingPlaybackStatus::Stopped);
    EXPECT(info.title.is_empty());
    EXPECT(info.artist.is_empty());
    EXPECT(info.album.is_empty());
    EXPECT(!info.duration.has_value());
    EXPECT(!info.elapsed_time.has_value());
    EXPECT_EQ(info.playback_rate.value(), 0);
    EXPECT(!info.commands.play);
    EXPECT(!info.commands.pause);
    EXPECT(!info.commands.toggle_play_pause);
    EXPECT(!info.commands.stop);
    EXPECT(!info.commands.next_track);
    EXPECT(!info.commands.previous_track);
    EXPECT(!info.commands.change_playback_position);
}

TEST_CASE(now_playing_extrapolates_position)
{
    auto info = now_playing_info_for(playing_state(), now);
    EXPECT(info.playback_status == NowPlayingPlaybackStatus::Playing);
    EXPECT_EQ(info.title, "Title"sv);
    EXPECT_EQ(info.artist, "Artist"sv);
    EXPECT_EQ(info.album, "Album"sv);
    EXPECT_EQ(info.duration.value(), 20);
    EXPECT_EQ(info.elapsed_time.value(), 7);
    EXPECT_EQ(info.playback_rate.value(), 1.5);
}

TEST_CASE(now_playing_clamps_position)
{
    auto state = playing_state();
    state.position->position = 19;
    EXPECT_EQ(now_playing_info_for(state, now).elapsed_time.value(), 20);
    state.position->position = 1;
    state.position->playback_rate = -1;
    EXPECT_EQ(now_playing_info_for(state, now).elapsed_time.value(), 0);
}

TEST_CASE(now_playing_paused_and_stopped_positions)
{
    auto state = playing_state();
    state.playback_state = MediaSessionPlaybackState::Paused;
    auto info = now_playing_info_for(state, now);
    EXPECT(info.playback_status == NowPlayingPlaybackStatus::Paused);
    EXPECT_EQ(info.elapsed_time.value(), 4);
    EXPECT_EQ(info.playback_rate.value(), 0);

    state.playback_state = MediaSessionPlaybackState::None;
    info = now_playing_info_for(state, now);
    EXPECT(info.playback_status == NowPlayingPlaybackStatus::Stopped);
    EXPECT_EQ(info.elapsed_time.value(), 4);
    EXPECT_EQ(info.playback_rate.value(), 0);
}

TEST_CASE(now_playing_without_position)
{
    auto state = playing_state();
    state.position.clear();
    auto info = now_playing_info_for(state, now);
    EXPECT(!info.duration.has_value());
    EXPECT(!info.elapsed_time.has_value());
    EXPECT_EQ(info.playback_rate.value(), 1);
    EXPECT(!info.commands.change_playback_position);

    state.playback_state = MediaSessionPlaybackState::Paused;
    EXPECT_EQ(now_playing_info_for(state, now).playback_rate.value(), 0);
}

TEST_CASE(now_playing_ignores_non_finite_position_values)
{
    for (double value : { AK::Infinity<double>, -AK::Infinity<double>, AK::NaN<double> }) {
        auto state = playing_state();
        state.position->duration = value;
        auto info = now_playing_info_for(state, now);
        EXPECT(!info.duration.has_value());
        EXPECT(!info.commands.change_playback_position);
        EXPECT_EQ(info.elapsed_time.value(), 7);

        state = playing_state();
        state.position->position = value;
        EXPECT(!now_playing_info_for(state, now).elapsed_time.has_value());

        state = playing_state();
        state.position->playback_rate = value;
        info = now_playing_info_for(state, now);
        EXPECT(!info.playback_rate.has_value());
        EXPECT(!info.elapsed_time.has_value());

        state.playback_state = MediaSessionPlaybackState::Paused;
        info = now_playing_info_for(state, now);
        EXPECT_EQ(info.playback_rate.value(), 0);
        EXPECT_EQ(info.elapsed_time.value(), 4);
    }
}

TEST_CASE(now_playing_default_commands)
{
    auto commands = now_playing_info_for(playing_state(), now).commands;
    EXPECT(commands.play);
    EXPECT(commands.pause);
    EXPECT(commands.toggle_play_pause);
    EXPECT(commands.stop);
    EXPECT(commands.change_playback_position);
    EXPECT(!commands.next_track);
    EXPECT(!commands.previous_track);
}

TEST_CASE(now_playing_commands_follow_supported_actions)
{
    auto state = playing_state();
    state.supported_actions = { MediaSessionAction::Nexttrack, MediaSessionAction::Previoustrack };
    auto commands = now_playing_info_for(state, now).commands;
    EXPECT(commands.next_track);
    EXPECT(commands.previous_track);
    EXPECT(!commands.play);
    EXPECT(!commands.pause);
    EXPECT(!commands.stop);
    EXPECT(!commands.change_playback_position);

    state.supported_actions = { MediaSessionAction::Nexttrack };
    commands = now_playing_info_for(state, now).commands;
    EXPECT(commands.next_track);
    EXPECT(!commands.previous_track);
    state.supported_actions = { MediaSessionAction::Previoustrack };
    commands = now_playing_info_for(state, now).commands;
    EXPECT(!commands.next_track);
    EXPECT(commands.previous_track);

    state.supported_actions.clear();
    commands = now_playing_info_for(state, now).commands;
    EXPECT(!commands.play);
    EXPECT(!commands.pause);
    EXPECT(!commands.stop);
    EXPECT(!commands.next_track);
    EXPECT(!commands.previous_track);
    EXPECT(!commands.change_playback_position);

    state.supported_actions = { MediaSessionAction::Play };
    commands = now_playing_info_for(state, now).commands;
    EXPECT(commands.play);
    EXPECT(!commands.pause);
}

TEST_CASE(now_playing_toggle_needs_play_and_pause)
{
    auto state = playing_state();
    state.supported_actions = { MediaSessionAction::Pause };
    auto commands = now_playing_info_for(state, now).commands;
    EXPECT(!commands.play);
    EXPECT(commands.pause);
    EXPECT(!commands.toggle_play_pause);
}
