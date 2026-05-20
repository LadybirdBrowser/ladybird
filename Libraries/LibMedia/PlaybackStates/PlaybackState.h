/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Types.h>

namespace Media {

enum class PlaybackState : u8 {
    Starting,
    Buffering,
    Playing,
    Paused,
    Seeking,
    Suspended,
};

constexpr StringView playback_state_to_string(PlaybackState state)
{
    switch (state) {
    case PlaybackState::Starting:
        return "Starting"sv;
    case PlaybackState::Buffering:
        return "Buffering"sv;
    case PlaybackState::Playing:
        return "Playing"sv;
    case PlaybackState::Paused:
        return "Paused"sv;
    case PlaybackState::Seeking:
        return "Seeking"sv;
    case PlaybackState::Suspended:
        return "Suspended"sv;
    }
    return "Invalid"sv;
}

}

namespace AK {

template<>
struct Formatter<Media::PlaybackState> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::PlaybackState state)
    {
        return Formatter<StringView>::format(builder, Media::playback_state_to_string(state));
    }
};

}
