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

    bool operator==(GpuFacts const&) const = default;
};

// Everything the blocklist decides from, collected by the system probe.
struct SystemFacts {
    Optional<GpuFacts> gpu;
    // Set instead of the facts when the probe crashed, hung or reported something unreadable. Never serialized.
    Optional<String> probe_failure;

    bool operator==(SystemFacts const&) const = default;

    JsonObject to_json() const;
    static ErrorOr<SystemFacts> from_json(StringView);
    static ErrorOr<SystemFacts> from_json(JsonObject const&);
};

}
