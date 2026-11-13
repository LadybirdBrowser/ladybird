/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibPlatformFeatures/SystemFacts.h>

namespace PlatformFeatures {

// Drivers name themselves however they like, so their strings are bounded before they reach reports. Vulkan's are at
// most 255 bytes, each of which may have become a three-byte replacement character.
static constexpr size_t maximum_string_length = 1024;

static JsonObject video_decode_to_json(VideoDecodeFacts const& facts)
{
    JsonObject object;
    object.set("supported"sv, facts.supported);
    object.set("dpb_and_output_coincide"sv, facts.dpb_and_output_coincide);
    object.set("dpb_and_output_distinct"sv, facts.dpb_and_output_distinct);
    object.set("dpb_supports_drm_format_modifiers"sv, facts.dpb_supports_drm_format_modifiers);
    return object;
}

static JsonObject ffmpeg_to_json(FFmpegFacts const& facts)
{
    JsonObject object;
    object.set("avcodec_version"sv, facts.avcodec_version);
    object.set("avutil_version"sv, facts.avutil_version);
    object.set("shares_bundled_struct_layouts"sv, facts.shares_bundled_struct_layouts);
    return object;
}

JsonObject SystemFacts::to_json() const
{
    JsonObject object;
    if (gpu.has_value()) {
        JsonObject gpu_object;
        gpu_object.set("vendor_id"sv, gpu->vendor_id);
        gpu_object.set("device_id"sv, gpu->device_id);
        gpu_object.set("device_name"sv, gpu->device_name);
        gpu_object.set("driver_id"sv, gpu->driver_id);
        gpu_object.set("driver_name"sv, gpu->driver_name);
        gpu_object.set("driver_info"sv, gpu->driver_info);
        gpu_object.set("driver_version"sv, gpu->driver_version);
        gpu_object.set("api_version"sv, gpu->api_version);
        gpu_object.set("device_identity"sv, gpu->device_identity);
        gpu_object.set("supports_ycbcr_sampling"sv, gpu->supports_ycbcr_sampling);
        if (gpu->cannot_decode_video_because.has_value())
            gpu_object.set("cannot_decode_video_because"sv, *gpu->cannot_decode_video_because);
        gpu_object.set("supports_video_maintenance2"sv, gpu->supports_video_maintenance2);
        gpu_object.set("h264"sv, video_decode_to_json(gpu->h264));
        gpu_object.set("h265"sv, video_decode_to_json(gpu->h265));
        gpu_object.set("vp9"sv, video_decode_to_json(gpu->vp9));
        gpu_object.set("av1"sv, video_decode_to_json(gpu->av1));
        object.set("gpu"sv, move(gpu_object));
    }
    if (bundled_ffmpeg.has_value())
        object.set("bundled_ffmpeg"sv, ffmpeg_to_json(*bundled_ffmpeg));
    if (system_ffmpeg.has_value())
        object.set("system_ffmpeg"sv, ffmpeg_to_json(*system_ffmpeg));
    return object;
}

static ErrorOr<bool> read_bool(JsonObject const& object, StringView key)
{
    auto value = object.get_bool(key);
    if (!value.has_value())
        return Error::from_string_literal("System facts are missing a boolean");
    return *value;
}

static ErrorOr<u32> read_u32(JsonObject const& object, StringView key)
{
    auto value = object.get_u32(key);
    if (!value.has_value())
        return Error::from_string_literal("System facts are missing a number");
    return *value;
}

static ErrorOr<String> read_string(JsonObject const& object, StringView key)
{
    auto value = object.get_string(key);
    if (!value.has_value())
        return Error::from_string_literal("System facts are missing a string");
    if (value->bytes().size() > maximum_string_length)
        return Error::from_string_literal("A string in the system facts is too long");
    return *value;
}

static ErrorOr<JsonObject const*> read_object(JsonObject const& object, StringView key)
{
    auto value = object.get_object(key);
    if (!value.has_value())
        return Error::from_string_literal("System facts are missing an object");
    return &value.value();
}

static ErrorOr<VideoDecodeFacts> video_decode_from_json(JsonObject const& object)
{
    VideoDecodeFacts facts;
    facts.supported = TRY(read_bool(object, "supported"sv));
    facts.dpb_and_output_coincide = TRY(read_bool(object, "dpb_and_output_coincide"sv));
    facts.dpb_and_output_distinct = TRY(read_bool(object, "dpb_and_output_distinct"sv));
    facts.dpb_supports_drm_format_modifiers = TRY(read_bool(object, "dpb_supports_drm_format_modifiers"sv));
    return facts;
}

static ErrorOr<FFmpegFacts> ffmpeg_from_json(JsonObject const& object)
{
    FFmpegFacts facts;
    facts.avcodec_version = TRY(read_u32(object, "avcodec_version"sv));
    facts.avutil_version = TRY(read_u32(object, "avutil_version"sv));
    facts.shares_bundled_struct_layouts = TRY(read_bool(object, "shares_bundled_struct_layouts"sv));
    return facts;
}

ErrorOr<SystemFacts> SystemFacts::from_json(StringView json)
{
    auto value = TRY(JsonValue::from_string(json));
    if (!value.is_object())
        return Error::from_string_literal("System facts are not an object");
    return from_json(value.as_object());
}

ErrorOr<SystemFacts> SystemFacts::from_json(JsonObject const& object)
{
    SystemFacts facts;
    if (object.has("gpu"sv)) {
        auto const& gpu_object = *TRY(read_object(object, "gpu"sv));
        GpuFacts gpu;
        gpu.vendor_id = TRY(read_u32(gpu_object, "vendor_id"sv));
        gpu.device_id = TRY(read_u32(gpu_object, "device_id"sv));
        gpu.device_name = TRY(read_string(gpu_object, "device_name"sv));
        gpu.driver_id = TRY(read_u32(gpu_object, "driver_id"sv));
        gpu.driver_name = TRY(read_string(gpu_object, "driver_name"sv));
        gpu.driver_info = TRY(read_string(gpu_object, "driver_info"sv));
        gpu.driver_version = TRY(read_u32(gpu_object, "driver_version"sv));
        gpu.api_version = TRY(read_u32(gpu_object, "api_version"sv));
        gpu.device_identity = TRY(read_string(gpu_object, "device_identity"sv));
        gpu.supports_ycbcr_sampling = TRY(read_bool(gpu_object, "supports_ycbcr_sampling"sv));
        if (gpu_object.has("cannot_decode_video_because"sv))
            gpu.cannot_decode_video_because = TRY(read_string(gpu_object, "cannot_decode_video_because"sv));
        gpu.supports_video_maintenance2 = TRY(read_bool(gpu_object, "supports_video_maintenance2"sv));
        gpu.h264 = TRY(video_decode_from_json(*TRY(read_object(gpu_object, "h264"sv))));
        gpu.h265 = TRY(video_decode_from_json(*TRY(read_object(gpu_object, "h265"sv))));
        gpu.vp9 = TRY(video_decode_from_json(*TRY(read_object(gpu_object, "vp9"sv))));
        gpu.av1 = TRY(video_decode_from_json(*TRY(read_object(gpu_object, "av1"sv))));
        facts.gpu = move(gpu);
    }
    if (object.has("bundled_ffmpeg"sv))
        facts.bundled_ffmpeg = TRY(ffmpeg_from_json(*TRY(read_object(object, "bundled_ffmpeg"sv))));
    if (object.has("system_ffmpeg"sv))
        facts.system_ffmpeg = TRY(ffmpeg_from_json(*TRY(read_object(object, "system_ffmpeg"sv))));
    return facts;
}

}
