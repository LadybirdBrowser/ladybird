/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AudioServer/AudioInputRingStream.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>

namespace AudioServer {

using AudioInputStreamID = u64;

struct RingStreamFormat {
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u32 channel_capacity { 0 };
    u64 capacity_frames { 0 };
};

struct AudioInputStreamDescriptor {
    AudioInputStreamID stream_id { 0 };
    RingStreamFormat format;
    StreamOverflowPolicy overflow_policy { StreamOverflowPolicy::DropOldest };
    Core::AnonymousBuffer shared_memory;
    IPC::File notify_fd;
};

}
