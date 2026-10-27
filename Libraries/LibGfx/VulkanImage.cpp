/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Array.h>
#    include <AK/Format.h>
#    include <AK/ScopeGuard.h>
#    include <AK/Vector.h>
#    include <LibGfx/Bitmap.h>
#    include <LibGfx/VulkanImage.h>
#    include <fcntl.h>
#    include <string.h>
#    include <unistd.h>

namespace Gfx {

static uint32_t find_memory_type_index(VkPhysicalDeviceMemoryProperties const& memory_properties, VkMemoryRequirements const& memory_requirements, VkMemoryPropertyFlags required_flags)
{
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        auto const property_flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((memory_requirements.memoryTypeBits & (1u << i)) && (property_flags & required_flags) == required_flags)
            return i;
    }

    return memory_properties.memoryTypeCount;
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

int VulkanImage::get_dma_buf_fd() const
{
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

static constexpr VkImageUsageFlags shared_image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static bool supports_dmabuf_image(VkPhysicalDevice physical_device, VkFormat format, uint64_t modifier, VkImageUsageFlags usage, VkExternalMemoryFeatureFlags required_features)
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkPhysicalDeviceExternalImageFormatInfo external_image_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modifier_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 image_format_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_image_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .flags = 0,
    };
    VkExternalImageFormatProperties external_image_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        .pNext = nullptr,
        .externalMemoryProperties = {},
    };
    VkImageFormatProperties2 image_format_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_image_properties,
        .imageFormatProperties = {},
    };
    if (vkGetPhysicalDeviceImageFormatProperties2(physical_device, &image_format_info, &image_format_properties) != VK_SUCCESS)
        return false;
    return (external_image_properties.externalMemoryProperties.externalMemoryFeatures & required_features) == required_features;
}

bool can_import_dmabuf_image(VkPhysicalDevice physical_device, VkFormat format, uint64_t modifier, VkImageUsageFlags usage)
{
    return supports_dmabuf_image(physical_device, format, modifier, usage, VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

Vector<uint64_t> renderable_drm_format_modifiers(VulkanContext const& context, VkFormat format)
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

    Vector<uint64_t> format_mods;
    for (VkDrmFormatModifierPropertiesEXT const& props : format_mod_props) {
        VkFormatFeatureFlags required_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.drmFormatModifierTilingFeatures & required_features) != required_features || props.drmFormatModifierPlaneCount != 1)
            continue;
        // NB: The image is shared as a dma-buf, which not every layout the device renders to can be.
        if (supports_dmabuf_image(context.physical_device, format, props.drmFormatModifier, shared_image_usage, VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            format_mods.append(props.drmFormatModifier);
    }
    return format_mods;
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_shared_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, ReadonlySpan<uint64_t> modifiers)
{
    // populate a list of all format modifiers that are both renderable, sampleable,
    // and accepted by the caller
    Vector<uint64_t> format_mods;
    for (auto modifier : renderable_drm_format_modifiers(context, format)) {
        if (modifiers.is_empty() || modifiers.contains_slow(modifier))
            format_mods.append(modifier);
    }

    // If no accepted DRM modifier supports a renderable image, fail here so higher-level
    // code can fall back to a different backing-store type.
    if (format_mods.is_empty())
        return Error::from_string_literal("no supported DRM format modifiers for shared image");

    NonnullRefPtr<VulkanImage> image = make_ref_counted<VulkanImage>(context);
    VkImageDrmFormatModifierListCreateInfoEXT image_drm_format_modifier_list_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifierCount = static_cast<uint32_t>(format_mods.size()),
        .pDrmFormatModifiers = format_mods.data(),
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
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = shared_image_usage,
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
    bool const is_linear_image = format_mods.size() == 1 && format_mods[0] == DRM_FORMAT_MOD_LINEAR;
    uint32_t mem_type_idx = mem_props.memoryTypeCount;

    if (is_linear_image) {
        mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mem_type_idx == mem_props.memoryTypeCount) {
            mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
    } else {
        mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    if (mem_type_idx == mem_props.memoryTypeCount) {
        return Error::from_string_literal("unable to find suitable image memory type");
    }

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
        .memoryTypeIndex = mem_type_idx,
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

    // NB: Our dma-bufs name a single plane at the start of its memory, as the importers expect.
    if (subresource_layout.offset != 0)
        return Error::from_string_literal("image plane does not start at the start of its memory");

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

ErrorOr<ImportedDmaBufImage> import_linux_dmabuf_image(VkPhysicalDevice physical_device, VkDevice device, PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties, LinuxDmaBufHandle const& dmabuf, VkFormat format, VkImageUsageFlags usage)
{
    if (dmabuf.objects.size() != 1 || dmabuf.planes.size() != 1 || dmabuf.size.is_empty())
        return Error::from_string_literal("dma-buf is not a single-plane image");
    if (!can_import_dmabuf_image(physical_device, format, dmabuf.modifier, usage))
        return Error::from_string_literal("The device cannot import the dma-buf's layout");

    ImportedDmaBufImage imported;
    ArmedScopeGuard destroy_on_failure = [&] {
        if (imported.image != VK_NULL_HANDLE)
            vkDestroyImage(device, imported.image, nullptr);
        if (imported.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, imported.memory, nullptr);
    };

    // The dma-buf names the exact layout of its pixels, so the image takes that layout rather than one the driver picks.
    auto const& plane = dmabuf.planes.first();
    VkSubresourceLayout plane_layout = {
        .offset = plane.offset,
        .size = 0,
        .rowPitch = plane.pitch,
        .arrayPitch = 0,
        .depthPitch = 0,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifier = dmabuf.modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };
    VkExternalMemoryImageCreateInfo external_memory_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &explicit_modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_memory_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = static_cast<uint32_t>(dmabuf.size.width()), .height = static_cast<uint32_t>(dmabuf.size.height()), .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(device, &image_info, nullptr, &imported.image) != VK_SUCCESS)
        return Error::from_string_literal("dma-buf image creation failed");

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device, imported.image, &memory_requirements);
    VkMemoryFdPropertiesKHR memory_fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        .pNext = nullptr,
        .memoryTypeBits = 0,
    };
    if (get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf.objects.first().fd(), &memory_fd_properties) != VK_SUCCESS)
        return Error::from_string_literal("dma-buf memory properties are unavailable");

    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    memory_requirements.memoryTypeBits &= memory_fd_properties.memoryTypeBits;
    auto memory_type_index = find_memory_type_index(memory_properties, memory_requirements, 0);
    if (memory_type_index == memory_properties.memoryTypeCount)
        return Error::from_string_literal("dma-buf memory cannot be imported");

    // NB: A successful import takes ownership of the descriptor it is given, so it is given a duplicate.
    int imported_fd = fcntl(dmabuf.objects.first().fd(), F_DUPFD_CLOEXEC, 0);
    if (imported_fd < 0)
        return Error::from_errno(errno);
    VkMemoryDedicatedAllocateInfo dedicated_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = imported.image,
        .buffer = VK_NULL_HANDLE,
    };
    VkImportMemoryFdInfoKHR import_memory_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_allocate_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_memory_info,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    if (vkAllocateMemory(device, &allocate_info, nullptr, &imported.memory) != VK_SUCCESS) {
        ::close(imported_fd);
        return Error::from_string_literal("dma-buf memory import failed");
    }
    if (vkBindImageMemory(device, imported.image, imported.memory, 0) != VK_SUCCESS)
        return Error::from_string_literal("dma-buf memory binding failed");

    destroy_on_failure.disarm();
    return imported;
}

ErrorOr<NonnullRefPtr<Bitmap>> read_linux_dmabuf_into_bitmap(VulkanContext const& context, LinuxDmaBufHandle const& dmabuf)
{
    if (dmabuf.drm_format != DRM_FORMAT_ARGB8888)
        return Error::from_string_literal("dma-buf is not a BGRA image");

    auto width = static_cast<uint32_t>(dmabuf.size.width());
    auto height = static_cast<uint32_t>(dmabuf.size.height());
    auto device = context.logical_device;

    auto imported = TRY(import_linux_dmabuf_image(context.physical_device, device, context.ext_procs.get_memory_fd_properties, dmabuf, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &memory_properties);
    auto image = imported.image;
    auto image_memory = imported.memory;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    ScopeGuard destroy_objects = [&] {
        if (buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, buffer, nullptr);
        if (buffer_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, buffer_memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, image_memory, nullptr);
    };

    auto row_size = static_cast<VkDeviceSize>(width) * 4;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = row_size * height,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        return Error::from_string_literal("readback buffer creation failed");
    VkMemoryRequirements buffer_memory_requirements;
    vkGetBufferMemoryRequirements(device, buffer, &buffer_memory_requirements);
    // NB: The CPU reads uncached memory very slowly, so cached memory is preferred.
    auto buffer_memory_type_index = find_memory_type_index(memory_properties, buffer_memory_requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (buffer_memory_type_index == memory_properties.memoryTypeCount)
        buffer_memory_type_index = find_memory_type_index(memory_properties, buffer_memory_requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buffer_memory_type_index == memory_properties.memoryTypeCount)
        return Error::from_string_literal("no host-visible memory for the readback buffer");
    bool buffer_memory_is_coherent = memory_properties.memoryTypes[buffer_memory_type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo buffer_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = buffer_memory_requirements.size,
        .memoryTypeIndex = buffer_memory_type_index,
    };
    if (vkAllocateMemory(device, &buffer_allocate_info, nullptr, &buffer_memory) != VK_SUCCESS)
        return Error::from_string_literal("readback buffer memory allocation failed");
    if (vkBindBufferMemory(device, buffer, buffer_memory, 0) != VK_SUCCESS)
        return Error::from_string_literal("readback buffer memory binding failed");

    vkResetCommandBuffer(context.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(context.command_buffer, &begin_info);
    // The exporter leaves its images in the general layout, and hands them over from another instance of the driver.
    VkImageMemoryBarrier acquire_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = context.graphics_queue_family,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(context.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire_barrier);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyImageToBuffer(context.command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(context.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host_barrier, 0, nullptr);
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
    if (vkQueueSubmit(context.graphics_queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS)
        return Error::from_string_literal("readback submission failed");
    if (vkQueueWaitIdle(context.graphics_queue) != VK_SUCCESS)
        return Error::from_string_literal("readback did not finish");

    void* pixels = nullptr;
    if (vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &pixels) != VK_SUCCESS)
        return Error::from_string_literal("readback buffer mapping failed");
    if (!buffer_memory_is_coherent) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = buffer_memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkInvalidateMappedMemoryRanges(device, 1, &range);
    }
    auto bitmap_or_error = Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, dmabuf.size);
    if (!bitmap_or_error.is_error()) {
        auto& bitmap = *bitmap_or_error.value();
        for (uint32_t y = 0; y < height; ++y)
            memcpy(bitmap.scanline_u8(static_cast<int>(y)), static_cast<u8 const*>(pixels) + y * row_size, row_size);
    }
    vkUnmapMemory(device, buffer_memory);
    return bitmap_or_error;
}

}

#endif
