/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN

#    include <LibGfx/VulkanContext.h>
#    include <core/SkRefCnt.h>
#    include <gpu/vk/VulkanMemoryAllocator.h>

namespace Gfx {

sk_sp<skgpu::VulkanMemoryAllocator> create_skia_vulkan_memory_allocator(VulkanContext const&);

}

#endif
