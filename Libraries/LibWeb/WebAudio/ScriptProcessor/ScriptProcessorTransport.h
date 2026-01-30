/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::WebAudio::Render {

static constexpr u32 script_processor_stream_version = 1;

static constexpr u32 script_processor_request_magic = 0x53505251u;  // "SPRQ"
static constexpr u32 script_processor_response_magic = 0x53505253u; // "SPRS"

struct ScriptProcessorRequestHeader {
    u32 magic { script_processor_request_magic };
    u32 version { script_processor_stream_version };

    u64 node_id { 0 };
    double playback_time_seconds { 0.0 };

    u32 buffer_size { 0 };
    u32 input_channel_count { 0 };
    u32 output_channel_count { 0 };
    u32 reserved0 { 0 };
};

struct ScriptProcessorResponseHeader {
    u32 magic { script_processor_response_magic };
    u32 version { script_processor_stream_version };

    u64 node_id { 0 };

    u32 buffer_size { 0 };
    u32 output_channel_count { 0 };
    u32 reserved0 { 0 };
    u32 reserved1 { 0 };
};

static constexpr size_t script_processor_request_fixed_bytes = sizeof(ScriptProcessorRequestHeader);
static constexpr size_t script_processor_response_fixed_bytes = sizeof(ScriptProcessorResponseHeader);

}
