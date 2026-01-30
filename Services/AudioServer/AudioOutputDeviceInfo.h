/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>

namespace AudioServer {

using AudioOutputDeviceID = u64;

struct AudioOutputDeviceInfo {
    AudioOutputDeviceID device_id { 0 };
    ByteString label;
    ByteString persistent_id;
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    bool is_default { false };
};

}
