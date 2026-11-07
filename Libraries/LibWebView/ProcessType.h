/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace WebView {

enum class ProcessType : u8 {
    // The UI and the Hub in one process, or the Hub with no UI.
    Browser,
    UI,
    Hub,
    Compositor,
    WebContent,
    RequestServer,
    ImageDecoder,
    MediaServer,
    AudioServer,
    VideoDecoderServer,
    WebRTCClient,
};

}
