/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <LibWebView/Export.h>

namespace WebView {

// The UNIX socket the daemon that has the cameras (PipeWire) answers on, found the way its library finds it.
WEBVIEW_API ErrorOr<ByteString> camera_daemon_socket_path();

}
