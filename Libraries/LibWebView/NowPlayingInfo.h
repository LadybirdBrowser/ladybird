/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/MediaSessionCoordinator.h>

namespace WebView {

enum class NowPlayingPlaybackStatus {
    Playing,
    Paused,
    Stopped,
};

struct NowPlayingInfo {
    NowPlayingPlaybackStatus playback_status { NowPlayingPlaybackStatus::Stopped };
    String title;
    String artist;
    String album;
    Optional<double> duration;
    Optional<double> elapsed_time;
    Optional<double> playback_rate { 0.0 };
    struct {
        bool play { false };
        bool pause { false };
        bool toggle_play_pause { false };
        bool stop { false };
        bool next_track { false };
        bool previous_track { false };
        bool change_playback_position { false };
    } commands;
};

WEBVIEW_API NowPlayingInfo now_playing_info_for(Optional<NowPlayingState> const&, MonotonicTime now);

}
