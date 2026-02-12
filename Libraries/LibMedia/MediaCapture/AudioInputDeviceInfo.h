/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>
#include <LibMedia/Export.h>

namespace Media::Capture {

using AudioInputDeviceID = u64;

struct MEDIA_API AudioInputDeviceInfo {
    AudioInputDeviceID device_id { 0 };
    ByteString label;
    ByteString persistent_id;
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    bool is_default { false };
};

}
