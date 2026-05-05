/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibCore/Environment.h>

namespace Audio {

inline bool should_log_audio_server()
{
    return Core::Environment::has("AUDIO_SERVER_LOG"sv);
}

}
