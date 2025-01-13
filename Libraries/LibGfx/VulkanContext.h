/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN

#    include <AK/Forward.h>
#    include <AK/Function.h>
#    include <AK/RefCounted.h>
#    include <vulkan/vulkan.h>

namespace Gfx {

struct VulkanContext : public RefCounted<VulkanContext> {
    uint32_t api_version { VK_API_VERSION_1_0 };
    VkInstance instance { VK_NULL_HANDLE };
    VkPhysicalDevice physical_device { VK_NULL_HANDLE };
    VkDevice logical_device { VK_NULL_HANDLE };
    VkQueue graphics_queue { VK_NULL_HANDLE };

    VulkanContext(uint32_t api_version, VkInstance instance, VkPhysicalDevice physical_device, VkDevice logical_device, VkQueue graphics_queue)
        : api_version(api_version)
        , instance(instance)
        , physical_device(physical_device)
        , logical_device(logical_device)
        , graphics_queue(graphics_queue)
    {
    }
};

ErrorOr<NonnullRefPtr<VulkanContext>> create_vulkan_context();

namespace Vulkan {

// TODO: Make this more RAII and less C
struct Image {
    VkDevice device { VK_NULL_HANDLE };
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkDeviceSize alloc_size { 0 };
    VkImageCreateInfo create_info {};
    int exported_fd = { -1 };
};

ErrorOr<Image> create_image(VulkanContext&, VkExtent2D, VkFormat);

}

}

#endif
