/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN

#    include <AK/Assertions.h>
#    include <AK/Debug.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefCounted.h>
#    include <vulkan/vulkan.h>
#    if defined(USE_VULKAN_IMAGES)
#        include <libdrm/drm_fourcc.h>
#    endif

namespace Gfx {

class VulkanContext : public RefCounted<VulkanContext> {
    friend class VulkanImage;

public:
    static ErrorOr<NonnullRefPtr<VulkanContext>> create();

    ~VulkanContext();

    uint32_t api_version() const { return m_api_version; }
    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physical_device() const { return m_physical_device; }
    VkDevice logical_device() const { return m_logical_device; }
    VkQueue graphics_queue() const { return m_graphics_queue; }
    uint32_t graphics_queue_family() const { return m_graphics_queue_family; }

private:
    VulkanContext();

    ErrorOr<void> create_instance();
    ErrorOr<void> pick_physical_device();
    ErrorOr<void> create_logical_device_and_queue();

#    ifdef USE_VULKAN_IMAGES
    ErrorOr<void> create_command_pool();
    ErrorOr<void> allocate_command_buffer();
    ErrorOr<void> get_extensions();
#    endif

#    if VULKAN_DEBUG
    bool check_layer_support(StringView layer);
#    endif

    uint32_t m_api_version { VK_API_VERSION_1_0 };
    VkInstance m_instance { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice m_logical_device { VK_NULL_HANDLE };
    VkQueue m_graphics_queue { VK_NULL_HANDLE };
    uint32_t m_graphics_queue_family { 0 };

#    ifdef USE_VULKAN_IMAGES
    VkCommandPool m_command_pool { VK_NULL_HANDLE };
    VkCommandBuffer m_command_buffer { VK_NULL_HANDLE };
    struct
    {
        PFN_vkGetMemoryFdKHR get_memory_fd { nullptr };
        PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties { nullptr };
    } m_ext_procs;
#    endif

#    if VULKAN_DEBUG
    VkDebugUtilsMessengerEXT m_debug_messenger { VK_NULL_HANDLE };
#    endif
};

#    ifdef USE_VULKAN_IMAGES
class VulkanImage : public RefCounted<VulkanImage> {
public:
    struct Info {
        VkFormat format;
        VkExtent3D extent;
        VkImageTiling tiling;
        VkImageUsageFlags usage;
        VkSharingMode sharing_mode;
        VkImageLayout layout;
        VkDeviceSize row_pitch; // for tiled images this is some implementation-specific value
        uint64_t modifier { DRM_FORMAT_MOD_INVALID };
    };

    static ErrorOr<NonnullRefPtr<VulkanImage>> create_shared(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, uint32_t num_modifiers, uint64_t const* modifiers);

    ~VulkanImage();

    int get_dma_buf_fd();
    void transition_layout(VkImageLayout old_layout, VkImageLayout new_layout);

    VkImage image() const { return m_image; }
    Info const& info() const { return m_info; }

private:
    VulkanImage(VulkanContext const& context);

    VkImage m_image { VK_NULL_HANDLE };
    VkDeviceMemory m_memory { VK_NULL_HANDLE };
    Info m_info {};

    VulkanContext const& m_context;
};

static inline uint32_t vk_format_to_drm_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DRM_FORMAT_ARGB8888;
    // add more as needed
    default:
        VERIFY_NOT_REACHED();
        return DRM_FORMAT_INVALID;
    }
}
#    endif

}

#endif
