/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace AudioServer {

// The socket the sandboxed AudioServer's audio library is pointed at. Nothing listens there: the Browser's connect
// broker knows the name and connects the socket to the audio daemon wherever that is at the time.
static constexpr StringView brokered_audio_daemon_path = "/ladybird/audio-daemon"sv;

}
