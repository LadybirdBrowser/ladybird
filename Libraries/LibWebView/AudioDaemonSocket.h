/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWebView/Export.h>

namespace WebView {

// The UNIX socket the audio daemon answers on right now, found by connecting as any client would and asking the
// library what it connected to rather than predicting its lookup. A daemon reached over TCP is an error.
WEBVIEW_API ErrorOr<ByteString> audio_daemon_socket_path();

// The socket a PulseAudio server string names, in whichever form it was configured, or nothing for a host name.
WEBVIEW_API Optional<ByteString> unix_path_from_server_string(StringView);

}
