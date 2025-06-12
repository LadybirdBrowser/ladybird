/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>

namespace Web::HTML {

enum class AudioPlayState {
    Paused,
    Playing,
};

enum class MuteState {
    Muted,
    Unmuted,
};

constexpr MuteState invert_mute_state(MuteState mute_state)
{
    switch (mute_state) {
    case MuteState::Muted:
        return MuteState::Unmuted;
    case MuteState::Unmuted:
        return MuteState::Muted;
    }

    VERIFY_NOT_REACHED();
}

}
