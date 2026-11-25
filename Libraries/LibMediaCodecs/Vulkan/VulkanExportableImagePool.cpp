/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibMediaCodecs/Vulkan/VulkanExportableImagePool.h>

namespace Media {

ErrorOr<NonnullRefPtr<VulkanExportableImagePool>> VulkanExportableImagePool::create(VulkanVideoDevice const& device, VkFormat format, Gfx::IntSize size, u32 drm_format)
{
    auto drm_format_modifiers = device.exportable_drm_format_modifiers(format, IMAGE_USAGE, 0, nullptr, size);
    if (drm_format_modifiers.is_empty())
        return Error::from_string_literal("No DRM format modifier can export images of this format");
    return adopt_ref(*new VulkanExportableImagePool(device, format, size, drm_format, move(drm_format_modifiers)));
}

VulkanExportableImagePool::VulkanExportableImagePool(VulkanVideoDevice const& device, VkFormat format, Gfx::IntSize size, u32 drm_format, Vector<u64> drm_format_modifiers)
    : m_device(device)
    , m_format(format)
    , m_size(size)
    , m_drm_format(drm_format)
    , m_drm_format_modifiers(move(drm_format_modifiers))
{
}

VulkanExportableImagePool::~VulkanExportableImagePool()
{
    // Every image taken out holds a reference to the pool, so all of them are back.
    for (auto const& free_image : m_free_images) {
        vkDestroyImage(m_device.logical_device(), free_image.image, nullptr);
        vkFreeMemory(m_device.logical_device(), free_image.memory, nullptr);
    }
}

VulkanExportableImagePool::Image::~Image()
{
    if (m_abandoned)
        return;
    MutexLocker locker { m_pool->m_mutex };
    m_pool->m_free_images.append({ m_image, m_memory, move(m_surface) });
}

ErrorOr<NonnullRefPtr<VulkanExportableImagePool::Image>> VulkanExportableImagePool::take_image()
{
    Optional<FreeImage> free_image;
    {
        MutexLocker locker { m_mutex };
        if (!m_free_images.is_empty())
            free_image = m_free_images.take_last();
    }
    if (!free_image.has_value())
        free_image = TRY(create_image());

    return adopt_ref(*new Image(*this, free_image->image, free_image->memory, free_image->surface));
}

ErrorOr<VulkanExportableImagePool::FreeImage> VulkanExportableImagePool::create_image()
{
    auto logical_device = m_device.logical_device();

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list {};
    modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    modifier_list.drmFormatModifierCount = m_drm_format_modifiers.size();
    modifier_list.pDrmFormatModifiers = m_drm_format_modifiers.data();

    VkExternalMemoryImageCreateInfo external_memory_info {};
    external_memory_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_memory_info.pNext = &modifier_list;
    external_memory_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // The modifiers were chosen for images shared between the device's queue families whenever it has several.
    Vector<u32> queue_family_indices;
    for (auto const& family : m_device.queue_families())
        queue_family_indices.append(family.index);

    VkImageCreateInfo image_info {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_memory_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = m_format;
    image_info.extent = { static_cast<u32>(m_size.width()), static_cast<u32>(m_size.height()), 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = IMAGE_USAGE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (queue_family_indices.size() > 1) {
        image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        image_info.queueFamilyIndexCount = queue_family_indices.size();
        image_info.pQueueFamilyIndices = queue_family_indices.data();
    }

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(logical_device, &image_info, nullptr, &image) != VK_SUCCESS)
        return Error::from_string_literal("Could not create an exportable image");
    ArmedScopeGuard destroy_image = [&] { vkDestroyImage(logical_device, image, nullptr); };

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(logical_device, image, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(m_device.physical_device(), &memory_properties);
    Optional<u32> memory_type_index;
    for (u32 index = 0; index < memory_properties.memoryTypeCount; index++) {
        if ((requirements.memoryTypeBits & (1u << index)) == 0)
            continue;
        if ((memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)
            continue;
        memory_type_index = index;
        break;
    }
    if (!memory_type_index.has_value())
        return Error::from_string_literal("No device memory can hold an exportable image");

    // Some drivers only export memory that is dedicated to its image.
    VkMemoryDedicatedAllocateInfo dedicated_info {};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.image = image;
    VkExportMemoryAllocateInfo export_info {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext = &dedicated_info;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo allocate_info {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = *memory_type_index;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(logical_device, &allocate_info, nullptr, &memory) != VK_SUCCESS)
        return Error::from_string_literal("Could not allocate memory for an exportable image");
    ArmedScopeGuard free_memory = [&] { vkFreeMemory(logical_device, memory, nullptr); };
    if (vkBindImageMemory(logical_device, image, memory, 0) != VK_SUCCESS)
        return Error::from_string_literal("Could not bind memory to an exportable image");

    auto dma_buf = TRY(m_device.export_image_as_dma_buf(image, memory, m_format, m_size, m_drm_format));
    auto surface = TRY(VideoSurface::create_from_dma_buf(get_random<u64>(), move(dma_buf)));

    free_memory.disarm();
    destroy_image.disarm();
    return FreeImage { image, memory, move(surface) };
}

}
