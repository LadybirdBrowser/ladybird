/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibPlatformFeatures/SystemFacts.h>

// Only builds that paint through Vulkan define this.
Optional<PlatformFeatures::GpuFacts> describe_gpu_through_vulkan(Optional<Gfx::GpuDeviceIdentity> const& preferred_device);
