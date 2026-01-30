/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AudioServer/AudioInputDeviceInfo.h>
#include <AudioServer/AudioInputStreamDescriptor.h>

namespace AudioServer {

class AudioInputStreamManager {
public:
    static ErrorOr<AudioInputStreamDescriptor> create_stream(AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, StreamOverflowPolicy overflow_policy);
    static void destroy_stream(AudioInputStreamID stream_id);
};

}
