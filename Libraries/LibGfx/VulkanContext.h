/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN

#    include <AK/Error.h>
#    include <AK/Span.h>
#    include <LibGfx/GpuDeviceIdentity.h>
#    include <vulkan/vulkan.h>

namespace Gfx {

struct VulkanContext {
    uint32_t api_version { VK_API_VERSION_1_0 };
    VkInstance instance { VK_NULL_HANDLE };
    VkPhysicalDevice physical_device { VK_NULL_HANDLE };
    GpuDeviceIdentity device_identity;
    // Whether this is the device the context was asked to prefer, which tells the context's user that whoever asked
    // can import the memory it shares in any layout this device renders to.
    bool is_preferred_device { false };
    VkDevice logical_device { VK_NULL_HANDLE };
    VkQueue graphics_queue { VK_NULL_HANDLE };
    uint32_t graphics_queue_family { 0 };
    ReadonlySpan<char const*> enabled_device_extensions;
    bool can_sample_through_ycbcr_conversion { false };
#    ifdef USE_VULKAN_DMABUF_IMAGES
    VkCommandPool command_pool { VK_NULL_HANDLE };
    VkCommandBuffer command_buffer { VK_NULL_HANDLE };
    struct
    {
        PFN_vkGetMemoryFdKHR get_memory_fd { nullptr };
        PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties { nullptr };
        PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties { nullptr };
    } ext_procs;
#    endif
};

// The preferred device is picked when it is present, since memory the context shares can only be imported on the same one.
ErrorOr<VulkanContext> create_vulkan_context(Optional<GpuDeviceIdentity> const& preferred_device = {});
GpuDeviceIdentity gpu_device_identity_of(VkPhysicalDevice);

}

#endif
