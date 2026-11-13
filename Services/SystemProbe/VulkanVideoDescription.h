/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/VulkanContext.h>
#include <LibPlatformFeatures/SystemFacts.h>

// Only builds that decode video through Vulkan define this.
void describe_video_decoding_through_vulkan(PlatformFeatures::GpuFacts&, VkInstance, VkPhysicalDevice);
