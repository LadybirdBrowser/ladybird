/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Assertions.h>
#    include <AK/Debug.h>
#    include <AK/Format.h>
#    include <AK/ScopeGuard.h>
#    include <AK/StdLibExtras.h>
#    include <AK/Vector.h>
#    include <LibGfx/Bitmap.h>
#    include <LibGfx/VulkanContext.h>
#    include <LibGfx/VulkanImage.h>
#    include <unistd.h>

namespace Gfx {

ErrorOr<VkFormat> drm_format_to_vk_format(u32 drm_format)
{
    switch (drm_format) {
    case DRM_FORMAT_ARGB8888:
        return VkFormat(VK_FORMAT_B8G8R8A8_UNORM);
    case DRM_FORMAT_ABGR8888:
        return VkFormat(VK_FORMAT_R8G8B8A8_UNORM);
    default:
        return Error::from_string_literal("unsupported drm format for dma-buf import");
    }
}

ErrorOr<BitmapFormat> vk_format_to_bitmap_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return BitmapFormat::BGRA8888;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return BitmapFormat::RGBA8888;
    default:
        return Error::from_string_literal("unsupported bitmap format for dma-buf import");
    }
}

uint32_t vk_format_to_drm_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DRM_FORMAT_ARGB8888;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return DRM_FORMAT_ABGR8888;
    default:
        return DRM_FORMAT_INVALID;
    }
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_dma_buf_vulkan_image(
    VulkanContext const& context,
    IntSize size,
    VkFormat format,
    VkMemoryPropertyFlags required_flags,
    VkMemoryPropertyFlags preferred_flags,
    ReadonlySpan<uint64_t> allowed_modifiers)
{
    VkDrmFormatModifierPropertiesListEXT format_mod_props_list = {};
    format_mod_props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    format_mod_props_list.pNext = nullptr;
    VkFormatProperties2 format_props = {};
    format_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_props.pNext = &format_mod_props_list;
    vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &format_props);
    Vector<VkDrmFormatModifierPropertiesEXT> format_mod_props;
    format_mod_props.resize(format_mod_props_list.drmFormatModifierCount);
    format_mod_props_list.pDrmFormatModifierProperties = format_mod_props.data();
    vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &format_props);

    // Populate a list of all format modifiers that are both renderable and accepted by the caller.
    // If there's no allow-list, accept any renderable single-plane modifier.
    Vector<uint64_t> renderable_modifiers;
    for (VkDrmFormatModifierPropertiesEXT const& props : format_mod_props) {
        if ((props.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            && props.drmFormatModifierPlaneCount == 1
            && (allowed_modifiers.is_empty() || allowed_modifiers.contains_slow(props.drmFormatModifier))) {

            renderable_modifiers.append(props.drmFormatModifier);
        }
    }
    if (renderable_modifiers.is_empty())
        return Error::from_string_literal("no suitable drm format modifiers for requested format");

    NonnullRefPtr<VulkanImage> image = make_ref_counted<VulkanImage>(context);
    VkImageDrmFormatModifierListCreateInfoEXT image_drm_format_modifier_list_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifierCount = static_cast<uint32_t>(renderable_modifiers.size()),
        .pDrmFormatModifiers = renderable_modifiers.data(),
    };
    VkExternalMemoryImageCreateInfo external_mem_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &image_drm_format_modifier_list_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    Array<uint32_t, 1> queue_families = { context.graphics_queue_family };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_mem_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = static_cast<uint32_t>(size.width()), .height = static_cast<uint32_t>(size.height()), .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = queue_families.size(),
        .pQueueFamilyIndices = queue_families.data(),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto result = vkCreateImage(context.logical_device, &image_info, nullptr, &image->image);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateImage returned {}", to_underlying(result));
        return Error::from_string_literal("image creation failed");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(context.logical_device, image->image, &mem_reqs);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &mem_props);

    Optional<uint32_t> preferred_mem_type_idx;
    Optional<uint32_t> required_mem_type_idx;
    for (uint32_t mem_type_idx = 0; mem_type_idx < mem_props.memoryTypeCount; ++mem_type_idx) {
        if (!(mem_reqs.memoryTypeBits & (1 << mem_type_idx)))
            continue;

        auto properties = mem_props.memoryTypes[mem_type_idx].propertyFlags;
        if ((properties & required_flags) != required_flags)
            continue;

        if (!required_mem_type_idx.has_value())
            required_mem_type_idx = mem_type_idx;

        if (preferred_flags != 0 && (properties & preferred_flags) == preferred_flags) {
            preferred_mem_type_idx = mem_type_idx;
            break;
        }
    }

    Optional<uint32_t> chosen_mem_type_idx = preferred_mem_type_idx.has_value() ? preferred_mem_type_idx : required_mem_type_idx;
    if (!chosen_mem_type_idx.has_value())
        return Error::from_string_literal("unable to find suitable image memory type");

    // Set up dedicated memory allocation; required for NVIDIA 10 series GPUs.
    // https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryAllocateInfo.html#VUID-VkMemoryAllocateInfo-pNext-00639
    VkMemoryDedicatedAllocateInfo mem_dedicated_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = image->image,
        .buffer = VK_NULL_HANDLE,
    };

    VkExportMemoryAllocateInfo export_mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &mem_dedicated_alloc_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_mem_alloc_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = chosen_mem_type_idx.value(),
    };
    result = vkAllocateMemory(context.logical_device, &mem_alloc_info, nullptr, &image->memory);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateMemory returned {}", to_underlying(result));
        return Error::from_string_literal("image memory allocation failed");
    }

    result = vkBindImageMemory(context.logical_device, image->image, image->memory, 0);
    if (result != VK_SUCCESS) {
        dbgln("vkBindImageMemory returned {}", to_underlying(result));
        return Error::from_string_literal("bind image memory failed");
    }

    VkImageSubresource subresource = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout subresource_layout = {};
    vkGetImageSubresourceLayout(context.logical_device, image->image, &subresource, &subresource_layout);

    VkImageDrmFormatModifierPropertiesEXT image_format_mod_props = {};
    image_format_mod_props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    image_format_mod_props.pNext = nullptr;
    result = context.ext_procs.get_image_drm_format_modifier_properties(context.logical_device, image->image, &image_format_mod_props);
    if (result != VK_SUCCESS) {
        dbgln("vkGetImageDrmFormatModifierPropertiesEXT returned {}", to_underlying(result));
        return Error::from_string_literal("image format modifier retrieval failed");
    }

    // external APIs require general layout
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    image->transition_layout(VK_IMAGE_LAYOUT_UNDEFINED, layout);

    image->info = {
        .format = image_info.format,
        .extent = image_info.extent,
        .tiling = image_info.tiling,
        .usage = image_info.usage,
        .sharing_mode = image_info.sharingMode,
        .layout = layout,
        .row_pitch = subresource_layout.rowPitch,
        .modifier = image_format_mod_props.drmFormatModifier,
    };
    return image;
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_dma_buf_vulkan_image(VulkanContext const& context, IntSize size, VkFormat format, bool cpu_mappable)
{
    AK::Vector<uint64_t> allowed_modifiers;
    if (cpu_mappable)
        allowed_modifiers.append(DRM_FORMAT_MOD_LINEAR);
    return create_dma_buf_vulkan_image(
        context,
        size,
        format,
        cpu_mappable ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        cpu_mappable ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0,
        allowed_modifiers);
}

ErrorOr<NonnullRefPtr<VulkanImage>> import_vulkan_image_from_dma_buf(VulkanContext const& context, IntSize size, VkFormat format, uint64_t modifier, LinuxDmaBufPlaneLayout const& plane, int fd)
{
    if (fd < 0)
        return Error::from_string_literal("dmabuf import: missing fd");

    ArmedScopeGuard close_fd_on_return([&] {
        if (fd >= 0)
            ::close(fd);
    });

    NonnullRefPtr<VulkanImage> image = make_ref_counted<VulkanImage>(context);

    VkSubresourceLayout plane_layout = {
        .offset = plane.offset,
        .size = 0,
        .rowPitch = plane.stride,
        .arrayPitch = 0,
        .depthPitch = 0,
    };

    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };

    VkExternalMemoryImageCreateInfo external_mem_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &explicit_modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    Array<uint32_t, 1> queue_families = { context.graphics_queue_family };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_mem_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = static_cast<uint32_t>(size.width()), .height = static_cast<uint32_t>(size.height()), .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = queue_families.size(),
        .pQueueFamilyIndices = queue_families.data(),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult result = vkCreateImage(context.logical_device, &image_info, nullptr, &image->image);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateImage returned {}", to_underlying(result));
        return Error::from_string_literal("dmabuf import: image creation failed");
    }

    auto destroy_image_on_error = ArmedScopeGuard([&] {
        if (image->image != VK_NULL_HANDLE) {
            vkDestroyImage(context.logical_device, image->image, nullptr);
            image->image = VK_NULL_HANDLE;
        }
    });

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(context.logical_device, image->image, &mem_reqs);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &mem_props);
    uint32_t mem_type_idx;
    for (mem_type_idx = 0; mem_type_idx < mem_props.memoryTypeCount; ++mem_type_idx) {
        if ((mem_reqs.memoryTypeBits & (1 << mem_type_idx)) && (mem_props.memoryTypes[mem_type_idx].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    }
    if (mem_type_idx == mem_props.memoryTypeCount)
        return Error::from_string_literal("dmabuf import: unable to find suitable image memory type");

    VkMemoryDedicatedAllocateInfo dedicated_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = image->image,
        .buffer = VK_NULL_HANDLE,
    };

    VkImportMemoryFdInfoKHR import_fd_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_alloc_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };

    VkMemoryAllocateInfo mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_fd_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_idx,
    };

    result = vkAllocateMemory(context.logical_device, &mem_alloc_info, nullptr, &image->memory);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateMemory returned {}", to_underlying(result));
        return Error::from_string_literal("dmabuf import: image memory allocation failed");
    }

    // Ownership of the fd has been transferred to Vulkan.
    fd = -1;

    result = vkBindImageMemory(context.logical_device, image->image, image->memory, 0);
    if (result != VK_SUCCESS) {
        dbgln("vkBindImageMemory returned {}", to_underlying(result));
        return Error::from_string_literal("dmabuf import: bind image memory failed");
    }

    VkImageSubresource subresource = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout subresource_layout = {};
    vkGetImageSubresourceLayout(context.logical_device, image->image, &subresource, &subresource_layout);

    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    image->transition_layout(VK_IMAGE_LAYOUT_UNDEFINED, layout);

    image->info = {
        .format = image_info.format,
        .extent = image_info.extent,
        .tiling = image_info.tiling,
        .usage = image_info.usage,
        .sharing_mode = image_info.sharingMode,
        .layout = layout,
        .row_pitch = subresource_layout.rowPitch,
        .modifier = modifier,
    };

    destroy_image_on_error.disarm();
    return image;
}

VulkanImage::VulkanImage(VulkanContext const& context)
    : context(context)
{
}

VulkanImage::~VulkanImage()
{
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(context.logical_device, image, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(context.logical_device, memory, nullptr);
    }
}

void VulkanImage::transition_layout(VkImageLayout old_layout, VkImageLayout new_layout)
{
    vkResetCommandBuffer(context.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(context.command_buffer, &begin_info);
    VkImageMemoryBarrier imageMemoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = 0,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(context.command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &imageMemoryBarrier);
    vkEndCommandBuffer(context.command_buffer);
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &context.command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vkQueueSubmit(context.graphics_queue, 1, &submit_info, nullptr);
    vkQueueWaitIdle(context.graphics_queue);
}

int VulkanImage::get_dma_buf_fd()
{
    if (memory == VK_NULL_HANDLE)
        return -1;

    VkMemoryGetFdInfoKHR get_fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd = -1;
    VkResult result = context.ext_procs.get_memory_fd(context.logical_device, &get_fd_info, &fd);
    if (result != VK_SUCCESS) {
        dbgln("vkGetMemoryFdKHR returned {}", to_underlying(result));
        return -1;
    }
    return fd;
}

}

#endif
