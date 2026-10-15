/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Array.h>
#    include <AK/BuiltinWrappers.h>
#    include <AK/ScopeGuard.h>
#    include <LibGfx/ImportedYCbCrVulkanImage.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <libdrm/drm_fourcc.h>
#    include <unistd.h>

namespace Gfx {

static Optional<VkFormat> ycbcr_vulkan_format_for_drm_format(u32 drm_format)
{
    switch (drm_format) {
    case DRM_FORMAT_NV12:
        return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    case DRM_FORMAT_NV16:
        return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
    case DRM_FORMAT_P010:
        return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    case DRM_FORMAT_P210:
        return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
    case DRM_FORMAT_P012:
        return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
    case DRM_FORMAT_P016:
        return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
    case DRM_FORMAT_YUV420:
        return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
    case DRM_FORMAT_YUV422:
        return VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM;
    case DRM_FORMAT_YUV444:
        return VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
    default:
        return {};
    }
}

static Optional<VkDrmFormatModifierPropertiesEXT> drm_format_modifier_properties(VkPhysicalDevice physical_device, VkFormat format, u64 modifier)
{
    VkDrmFormatModifierPropertiesListEXT modifier_list {};
    modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 format_properties {};
    format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_properties.pNext = &modifier_list;
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &format_properties);

    Vector<VkDrmFormatModifierPropertiesEXT> modifiers;
    modifiers.resize(modifier_list.drmFormatModifierCount);
    modifier_list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &format_properties);

    for (size_t i = 0; i < modifier_list.drmFormatModifierCount; i++) {
        if (modifiers[i].drmFormatModifier == modifier)
            return modifiers[i];
    }
    return {};
}

static ErrorOr<VkExternalMemoryFeatureFlags> external_memory_features_for_import(VulkanContext const& context, VkFormat format, u64 modifier, VkImageCreateFlags flags, IntSize size)
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info {};
    modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    modifier_info.drmFormatModifier = modifier;
    modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkPhysicalDeviceExternalImageFormatInfo external_info {};
    external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external_info.pNext = &modifier_info;
    external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkPhysicalDeviceImageFormatInfo2 format_info {};
    format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    format_info.pNext = &external_info;
    format_info.format = format;
    format_info.type = VK_IMAGE_TYPE_2D;
    format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    format_info.usage = ImportedYCbCrVulkanImage::USAGE;
    format_info.flags = flags;

    VkExternalImageFormatProperties external_properties {};
    external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    properties.pNext = &external_properties;
    if (vkGetPhysicalDeviceImageFormatProperties2(context.physical_device, &format_info, &properties) != VK_SUCCESS)
        return Error::from_string_literal("The Vulkan device cannot create an image of the dma-buf's format and modifier");

    auto const& maximum_extent = properties.imageFormatProperties.maxExtent;
    if (static_cast<u32>(size.width()) > maximum_extent.width || static_cast<u32>(size.height()) > maximum_extent.height)
        return Error::from_string_literal("The dma-buf image is larger than the Vulkan device allows");

    auto features = external_properties.externalMemoryProperties.externalMemoryFeatures;
    if ((features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0)
        return Error::from_string_literal("The Vulkan device cannot import the dma-buf's format and modifier");
    return features;
}

static ErrorOr<ImportedYCbCrVulkanImage::ImportedMemory> import_dma_buf_memory(VulkanContext const& context, int dma_buf_fd, VkMemoryRequirements const& requirements, VkImage dedicated_image)
{
    // The dma-buf comes from another process, so it must be shown to be large enough to back the image.
    auto dma_buf_size = lseek(dma_buf_fd, 0, SEEK_END);
    if (dma_buf_size < 0)
        return Error::from_errno(errno);
    if (static_cast<u64>(dma_buf_size) < requirements.size)
        return Error::from_string_literal("The dma-buf is smaller than the image it is said to hold");

    VkMemoryFdPropertiesKHR fd_properties {};
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (context.ext_procs.get_memory_fd_properties(context.logical_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf_fd, &fd_properties) != VK_SUCCESS)
        return Error::from_string_literal("Could not query the dma-buf's memory properties");
    auto memory_type_bits = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    if (memory_type_bits == 0)
        return Error::from_string_literal("The dma-buf's memory cannot back the image");

    auto fd = fcntl(dma_buf_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return Error::from_errno(errno);
    ArmedScopeGuard close_fd = [&] { close(fd); };

    VkMemoryDedicatedAllocateInfo dedicated_info {};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.image = dedicated_image;

    VkImportMemoryFdInfoKHR import_info {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    if (dedicated_image != VK_NULL_HANDLE)
        import_info.pNext = &dedicated_info;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = fd;

    VkMemoryAllocateInfo allocate_info {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &import_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = count_trailing_zeroes(memory_type_bits);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(context.logical_device, &allocate_info, nullptr, &memory) != VK_SUCCESS)
        return Error::from_string_literal("Could not import the dma-buf's memory");
    // A successful import takes ownership of the descriptor.
    close_fd.disarm();
    return ImportedYCbCrVulkanImage::ImportedMemory { memory, requirements.size };
}

enum class PlaneLayoutSource {
    ChosenByDriver,
    GivenExplicitly,
};

static ErrorOr<VkImage> create_image(VulkanContext const& context, VkFormat format, VkImageCreateFlags flags, LinuxDmaBufHandle const& dma_buf, PlaneLayoutSource layout_source)
{
    VkImageDrmFormatModifierListCreateInfoEXT modifier_list {};
    modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    modifier_list.drmFormatModifierCount = 1;
    modifier_list.pDrmFormatModifiers = &dma_buf.modifier;

    Array<VkSubresourceLayout, LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT> plane_layouts {};
    for (size_t plane_index = 0; plane_index < dma_buf.planes.size(); plane_index++) {
        plane_layouts[plane_index].offset = dma_buf.planes[plane_index].offset;
        plane_layouts[plane_index].rowPitch = dma_buf.planes[plane_index].pitch;
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier {};
    explicit_modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    explicit_modifier.drmFormatModifier = dma_buf.modifier;
    explicit_modifier.drmFormatModifierPlaneCount = dma_buf.planes.size();
    explicit_modifier.pPlaneLayouts = plane_layouts.data();

    VkExternalMemoryImageCreateInfo external_info {};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    if (layout_source == PlaneLayoutSource::ChosenByDriver)
        external_info.pNext = &modifier_list;
    else
        external_info.pNext = &explicit_modifier;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo image_info {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.flags = flags;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = { static_cast<u32>(dma_buf.size.width()), static_cast<u32>(dma_buf.size.height()), 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = ImportedYCbCrVulkanImage::USAGE;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(context.logical_device, &image_info, nullptr, &image) != VK_SUCCESS)
        return Error::from_string_literal("Could not create an image to import the dma-buf into");
    return image;
}

static bool image_is_laid_out_as_exported(VkDevice device, VkImage image, LinuxDmaBufHandle const& dma_buf)
{
    for (size_t plane_index = 0; plane_index < dma_buf.planes.size(); plane_index++) {
        VkImageSubresource subresource {};
        subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane_index;
        VkSubresourceLayout layout {};
        vkGetImageSubresourceLayout(device, image, &subresource, &layout);
        auto const& plane = dma_buf.planes[plane_index];
        if (layout.offset != plane.offset || layout.rowPitch != plane.pitch)
            return false;
    }
    return true;
}

// A modifier fixes how a device lays out an image of a given size and format, so the driver's own layout matches the
// exporter's whenever both images were made alike. Explicit plane layouts are the only way to describe one that does
// not match, but NVIDIA's driver (615.71) misplaces the planes of an image given them under a tiled modifier, so they
// are given only for linear images, where the pitch is the exporter's to choose.
static ErrorOr<VkImage> create_image_laid_out_as_exported(VulkanContext const& context, VkFormat format, VkImageCreateFlags flags, LinuxDmaBufHandle const& dma_buf)
{
    auto image = TRY(create_image(context, format, flags, dma_buf, PlaneLayoutSource::ChosenByDriver));
    if (image_is_laid_out_as_exported(context.logical_device, image, dma_buf))
        return image;
    vkDestroyImage(context.logical_device, image, nullptr);

    if (dma_buf.modifier != DRM_FORMAT_MOD_LINEAR)
        return Error::from_string_literal("The Vulkan device lays out the dma-buf's tiled image differently from its exporter");
    image = TRY(create_image(context, format, flags, dma_buf, PlaneLayoutSource::GivenExplicitly));
    if (image_is_laid_out_as_exported(context.logical_device, image, dma_buf))
        return image;
    vkDestroyImage(context.logical_device, image, nullptr);
    return Error::from_string_literal("The Vulkan device cannot lay out the dma-buf's image as its exporter did");
}

ErrorOr<NonnullOwnPtr<ImportedYCbCrVulkanImage>> ImportedYCbCrVulkanImage::import_dma_buf(VulkanContext const& context, LinuxDmaBufHandle const& dma_buf)
{
    if (!context.can_sample_through_ycbcr_conversion)
        return Error::from_string_literal("The Vulkan device cannot sample through a YCbCr conversion");
    if (dma_buf.size.is_empty())
        return Error::from_string_literal("The dma-buf image is empty");
    auto format = ycbcr_vulkan_format_for_drm_format(dma_buf.drm_format);
    if (!format.has_value())
        return Error::from_string_literal("The dma-buf's format has no YCbCr Vulkan format");

    auto modifier_properties = drm_format_modifier_properties(context.physical_device, *format, dma_buf.modifier);
    if (!modifier_properties.has_value())
        return Error::from_string_literal("The Vulkan device does not support the dma-buf's modifier for its format");
    if (dma_buf.planes.size() != modifier_properties->drmFormatModifierPlaneCount)
        return Error::from_string_literal("The dma-buf image has the wrong number of planes for its format and modifier");
    auto format_features = modifier_properties->drmFormatModifierTilingFeatures;
    if ((format_features & REQUIRED_FORMAT_FEATURES) != REQUIRED_FORMAT_FEATURES)
        return Error::from_string_literal("The Vulkan device cannot sample the dma-buf's format and modifier");

    // Planes in one dma-buf share one memory, while planes in separate dma-bufs need an image that binds them apart.
    auto is_disjoint = false;
    for (auto const& plane : dma_buf.planes) {
        if (plane.object_index >= dma_buf.objects.size())
            return Error::from_string_literal("A dma-buf image plane refers to an object it does not have");
        if (plane.object_index != 0)
            is_disjoint = true;
    }
    if (is_disjoint && (format_features & VK_FORMAT_FEATURE_DISJOINT_BIT) == 0)
        return Error::from_string_literal("The Vulkan device cannot bind the dma-buf image's planes apart");
    VkImageCreateFlags flags = is_disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;

    auto external_memory_features = TRY(external_memory_features_for_import(context, *format, dma_buf.modifier, flags, dma_buf.size));
    auto requires_dedicated_memory = (external_memory_features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    if (is_disjoint && requires_dedicated_memory)
        return Error::from_string_literal("The Vulkan device can import the dma-buf image only into dedicated memory, which its separate planes cannot have");

    auto imported = adopt_own(*new ImportedYCbCrVulkanImage(context.logical_device));
    imported->m_format = *format;
    imported->m_format_features = format_features;
    imported->m_is_disjoint = is_disjoint;

    imported->m_image = TRY(create_image_laid_out_as_exported(context, *format, flags, dma_buf));

    if (!is_disjoint) {
        VkImageMemoryRequirementsInfo2 requirements_info {};
        requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
        requirements_info.image = imported->m_image;
        VkMemoryDedicatedRequirements dedicated_requirements {};
        dedicated_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
        VkMemoryRequirements2 requirements {};
        requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        requirements.pNext = &dedicated_requirements;
        vkGetImageMemoryRequirements2(context.logical_device, &requirements_info, &requirements);

        auto uses_dedicated_memory = requires_dedicated_memory || dedicated_requirements.requiresDedicatedAllocation || dedicated_requirements.prefersDedicatedAllocation;
        auto memory = TRY(import_dma_buf_memory(context, dma_buf.objects[0].fd(), requirements.memoryRequirements, uses_dedicated_memory ? imported->m_image : VK_NULL_HANDLE));
        imported->m_memories.append(memory);
        if (vkBindImageMemory(context.logical_device, imported->m_image, memory.memory, 0) != VK_SUCCESS)
            return Error::from_string_literal("Could not bind the dma-buf's memory to the image");
        return imported;
    }

    Array<VkBindImagePlaneMemoryInfo, LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT> plane_bindings {};
    Array<VkBindImageMemoryInfo, LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT> bindings {};
    for (size_t plane_index = 0; plane_index < dma_buf.planes.size(); plane_index++) {
        auto aspect = static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane_index);

        VkImagePlaneMemoryRequirementsInfo plane_requirements_info {};
        plane_requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO;
        plane_requirements_info.planeAspect = aspect;
        VkImageMemoryRequirementsInfo2 requirements_info {};
        requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
        requirements_info.pNext = &plane_requirements_info;
        requirements_info.image = imported->m_image;
        VkMemoryRequirements2 requirements {};
        requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        vkGetImageMemoryRequirements2(context.logical_device, &requirements_info, &requirements);

        auto const& plane = dma_buf.planes[plane_index];
        auto memory = TRY(import_dma_buf_memory(context, dma_buf.objects[plane.object_index].fd(), requirements.memoryRequirements, VK_NULL_HANDLE));
        imported->m_memories.append(memory);

        plane_bindings[plane_index].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
        plane_bindings[plane_index].planeAspect = aspect;
        bindings[plane_index].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
        bindings[plane_index].pNext = &plane_bindings[plane_index];
        bindings[plane_index].image = imported->m_image;
        bindings[plane_index].memory = memory.memory;
    }
    if (vkBindImageMemory2(context.logical_device, dma_buf.planes.size(), bindings.data()) != VK_SUCCESS)
        return Error::from_string_literal("Could not bind the dma-buf's planes to the image");
    return imported;
}

ImportedYCbCrVulkanImage::~ImportedYCbCrVulkanImage()
{
    if (m_image != VK_NULL_HANDLE)
        vkDestroyImage(m_device, m_image, nullptr);
    for (auto const& memory : m_memories)
        vkFreeMemory(m_device, memory.memory, nullptr);
}

}

#endif
