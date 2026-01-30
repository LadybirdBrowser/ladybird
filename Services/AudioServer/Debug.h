/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Environment.h>

namespace AudioServer {

inline bool should_log_audio_server()
{
    return Core::Environment::has("AUDIO_SERVER_LOG"sv);
}

inline bool should_log_audio_server_ipc()
{
    return Core::Environment::has("AUDIO_SERVER_IPC_LOG"sv);
}

}
