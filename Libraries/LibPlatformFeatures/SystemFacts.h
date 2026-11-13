/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>

namespace PlatformFeatures {

// What the GPU can do when decoding one codec through Vulkan, for its most common 8-bit 4:2:0 profile.
struct VideoDecodeFacts {
    bool supported { false };
    // VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR and _DISTINCT_BIT_KHR.
    bool dpb_and_output_coincide { false };
    bool dpb_and_output_distinct { false };
    // Whether any format for images used only as references can be tiled under a DRM format modifier.
    bool dpb_supports_drm_format_modifiers { false };

    bool operator==(VideoDecodeFacts const&) const = default;
};

// The GPU that presents, as Vulkan describes it.
struct GpuFacts {
    u32 vendor_id { 0 };
    u32 device_id { 0 };
    String device_name;
    // VkDriverId, which tells apart drivers for the same vendor's hardware.
    u32 driver_id { 0 };
    String driver_name;
    String driver_info;
    // Encoded by each driver in its own way, so it is only compared alongside the driver ID.
    u32 driver_version { 0 };
    u32 api_version { 0 };
    String device_identity;

    bool supports_ycbcr_sampling { false };
    // Set when the device lacks something decoding through FFmpeg needs.
    Optional<String> cannot_decode_video_because;
    bool supports_video_maintenance2 { false };
    VideoDecodeFacts h264;
    VideoDecodeFacts h265;
    VideoDecodeFacts vp9;
    VideoDecodeFacts av1;

    bool operator==(GpuFacts const&) const = default;
};

struct FFmpegFacts {
    // The libraries' versions are packed like FFmpeg's AV_VERSION_INT.
    static constexpr u32 version(u32 major, u32 minor, u32 micro) { return major << 16 | minor << 8 | micro; }

    u32 avcodec_version { 0 };
    u32 avutil_version { 0 };
    // Whether the library lays out the structs the decoders reach into the way the bundled one does.
    bool shares_bundled_struct_layouts { false };

    bool operator==(FFmpegFacts const&) const = default;
};

// Everything the blocklist decides from, collected by the system probe.
struct SystemFacts {
    Optional<GpuFacts> gpu;
    Optional<FFmpegFacts> bundled_ffmpeg;
    Optional<FFmpegFacts> system_ffmpeg;
    // Set instead of the facts when the probe crashed, hung or reported something unreadable. Never serialized.
    Optional<String> probe_failure;

    bool operator==(SystemFacts const&) const = default;

    JsonObject to_json() const;
    static ErrorOr<SystemFacts> from_json(StringView);
    static ErrorOr<SystemFacts> from_json(JsonObject const&);
};

}
