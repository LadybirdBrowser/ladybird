/*
 * Copyright (c) 2025, Mehran Kamal <me@mehrankamal.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>

namespace Web::Platform {

enum MediaDeviceKind : u8 {
    VideoInput,
    AudioInput,
    AudioOutput,
};

struct MediaDevice {
    MediaDeviceKind kind {};
    String label;
    bool is_default;
};

}
