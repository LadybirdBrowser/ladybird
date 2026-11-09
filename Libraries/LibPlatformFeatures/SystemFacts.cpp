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
        object.set("gpu"sv, move(gpu_object));
    }
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
        auto gpu_object = object.get_object("gpu"sv);
        if (!gpu_object.has_value())
            return Error::from_string_literal("The GPU's system facts are not an object");
        GpuFacts gpu;
        gpu.vendor_id = TRY(read_u32(*gpu_object, "vendor_id"sv));
        gpu.device_id = TRY(read_u32(*gpu_object, "device_id"sv));
        gpu.device_name = TRY(read_string(*gpu_object, "device_name"sv));
        gpu.driver_id = TRY(read_u32(*gpu_object, "driver_id"sv));
        gpu.driver_name = TRY(read_string(*gpu_object, "driver_name"sv));
        gpu.driver_info = TRY(read_string(*gpu_object, "driver_info"sv));
        gpu.driver_version = TRY(read_u32(*gpu_object, "driver_version"sv));
        gpu.api_version = TRY(read_u32(*gpu_object, "api_version"sv));
        gpu.device_identity = TRY(read_string(*gpu_object, "device_identity"sv));
        gpu.supports_ycbcr_sampling = TRY(read_bool(*gpu_object, "supports_ycbcr_sampling"sv));
        facts.gpu = move(gpu);
    }
    return facts;
}

}
