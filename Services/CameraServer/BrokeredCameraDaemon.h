/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace CameraServer {

// The socket the sandboxed CameraServer's PipeWire library is pointed at. Nothing listens there: the Browser's connect
// broker knows the name and connects the socket to the PipeWire daemon wherever that is at the time.
static constexpr StringView brokered_camera_daemon_path = "/ladybird/camera-daemon"sv;

}
