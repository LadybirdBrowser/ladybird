/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Array.h>
#    include <AK/Checked.h>
#    include <AK/Format.h>
#    include <AK/ScopeGuard.h>
#    include <AK/Vector.h>
#    include <LibGfx/Bitmap.h>
#    include <LibGfx/DecodedImageFrame.h>
#    include <LibGfx/PaintingSurface.h>
#    include <LibGfx/SharedImage.h>
#    include <LibGfx/SharedImagePayload.h>
#    include <LibGfx/SkiaBackendContext.h>
#    include <LibGfx/VulkanImage.h>
#    include <LibIPC/File.h>
#    include <core/SkCanvas.h>
#    include <errno.h>
#    include <linux/dma-buf.h>
#    include <sys/ioctl.h>
#    include <sys/mman.h>
#    include <unistd.h>

namespace Gfx {

static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf(BitmapInfo const& info, LinuxDmaBufPayload const& dmabuf);
static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf_payload(SharedImagePayload const& payload);
static ErrorOr<NonnullRefPtr<SkiaBackendContext>> shared_image_skia_backend_context();
static ErrorOr<void> upload_decoded_image_frame_to_surface(DecodedImageFrame const& frame, PaintingSurface& painting_surface);
static LinuxDmaBufPayload create_linux_dmabuf_payload(VulkanImage const& vulkan_image, BitmapInfo const& info);
static bool dmabuf_sync(int fd, unsigned flags);

SharedImage::SharedImage(LinuxDmaBufPayload&& dma_buf_payload, BitmapInfo const& info, NonnullRefPtr<Bitmap> bitmap)
    : m_backing_kind(BackingKind::LinuxDmaBuf)
    , m_info(info)
    , m_bitmap(move(bitmap))
    , m_linux_dma_buf_payload(move(dma_buf_payload))
{
}

SharedImage::SharedImage(NonnullRefPtr<VulkanImage> vulkan_image, BitmapInfo const& info, NonnullRefPtr<Bitmap> bitmap)
    : m_backing_kind(BackingKind::VulkanImage)
    , m_info(info)
    , m_bitmap(move(bitmap))
    , m_vulkan_image(move(vulkan_image))
{
}

SharedImage SharedImage::create(BitmapInfo const& info)
{
    return create_bitmap_backed(info, info.alpha_type);
}

static SharedImagePayload duplicate_linux_dmabuf_payload(SharedImagePayload const& payload, ColorSpace color_space = {})
{
    auto const* dmabuf = payload.linux_dma_buf_payload();
    VERIFY(dmabuf);
    return SharedImagePayload(payload.info(), LinuxDmaBufPayload {
                                                  .drm_format = dmabuf->drm_format,
                                                  .stride = dmabuf->stride,
                                                  .offset = dmabuf->offset,
                                                  .file = MUST(IPC::File::clone_fd(dmabuf->file.fd())),
                                              },
        move(color_space));
}

SharedImage SharedImage::import_from_payload(SharedImagePayload payload)
{
    if (auto* shareable_bitmap = payload.shareable_bitmap()) {
        auto bitmap_info = payload.info();
        bitmap_info.row_bytes = static_cast<u32>(shareable_bitmap->bitmap()->pitch());
        auto shared_image = SharedImage(bitmap_info, NonnullRefPtr<Bitmap> { *shareable_bitmap->bitmap() });
        shared_image.set_color_space(payload.color_space());
        return shared_image;
    }

    auto duplicated_payload = duplicate_linux_dmabuf_payload(payload, payload.color_space());
    auto const* dma_buf_payload = duplicated_payload.linux_dma_buf_payload();
    VERIFY(dma_buf_payload);
    auto bitmap = create_bitmap_from_linux_dmabuf_payload(payload);
    auto bitmap_info = payload.info();
    bitmap_info.row_bytes = static_cast<u32>(bitmap->pitch());
    auto shared_image = SharedImage(LinuxDmaBufPayload {
                                        .drm_format = dma_buf_payload->drm_format,
                                        .stride = dma_buf_payload->stride,
                                        .offset = dma_buf_payload->offset,
                                        .file = MUST(IPC::File::clone_fd(dma_buf_payload->file.fd())),
                                    },
        bitmap_info, move(bitmap));
    shared_image.set_color_space(payload.color_space());
    return shared_image;
}

SharedImagePayload SharedImage::export_payload() const
{
    if (m_backing_kind == BackingKind::ShareableBitmap)
        return make_shareable_bitmap_payload(m_info, m_bitmap, m_color_space);

    if (m_backing_kind == BackingKind::LinuxDmaBuf) {
        auto payload_info = m_info;
        payload_info.row_bytes = m_linux_dma_buf_payload.stride;
        return SharedImagePayload(payload_info, LinuxDmaBufPayload {
                                                    .drm_format = m_linux_dma_buf_payload.drm_format,
                                                    .stride = m_linux_dma_buf_payload.stride,
                                                    .offset = m_linux_dma_buf_payload.offset,
                                                    .file = MUST(IPC::File::clone_fd(m_linux_dma_buf_payload.file.fd())),
                                                },
            m_color_space);
    }

    VERIFY(m_backing_kind == BackingKind::VulkanImage);
    VERIFY(m_vulkan_image);
    auto payload_info = m_info;
    payload_info.row_bytes = static_cast<u32>(m_vulkan_image->info.row_pitch);
    return SharedImagePayload(payload_info, create_linux_dmabuf_payload(*m_vulkan_image, payload_info), m_color_space);
}

SharedImage SharedImage::create_from_vulkan_image(NonnullRefPtr<VulkanImage> vulkan_image, BitmapInfo const& info)
{
    auto payload_info = info;
    payload_info.row_bytes = static_cast<u32>(vulkan_image->info.row_pitch);
    auto payload = SharedImagePayload(payload_info, create_linux_dmabuf_payload(*vulkan_image, payload_info));
    auto bitmap = create_bitmap_from_linux_dmabuf_payload(payload);
    auto bitmap_info = info;
    bitmap_info.row_bytes = static_cast<u32>(bitmap->pitch());
    return SharedImage(move(vulkan_image), bitmap_info, move(bitmap));
}

SharedImage::SharedImage(SharedImage&&) = default;

SharedImage& SharedImage::operator=(SharedImage&&) = default;

SharedImage::~SharedImage() = default;

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
        cpu_mappable ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        cpu_mappable ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0,
        allowed_modifiers);
}

ErrorOr<NonnullRefPtr<VulkanImage>> import_vulkan_image_from_dma_buf(VulkanContext const& context, IntSize size, VkFormat format, uint64_t modifier, DmaBufPlaneLayout const& plane, int fd)
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

ErrorOr<void> upload_decoded_image_frame_to_shared_image(DecodedImageFrame const& frame, SharedImagePayload& payload)
{
    if (auto* shareable_bitmap = payload.shareable_bitmap()) {
        auto* destination_bitmap = shareable_bitmap->bitmap();
        if (!destination_bitmap)
            return Error::from_string_literal("VulkanImage: shareable_bitmap_missing_destination_bitmap");
        if (destination_bitmap->size() != frame.size())
            return Error::from_string_literal("VulkanImage: shareable_bitmap_size_mismatch");

        return upload_decoded_image_frame_to_bitmap(frame, *destination_bitmap, payload.info(), payload.color_space());
    }

    auto const* dmabuf = payload.linux_dma_buf_payload();
    if (!dmabuf)
        return Error::from_string_literal("VulkanImage: missing_linux_dma_buf_payload");

    auto context = shared_image_skia_backend_context();
    if (!context.is_error()) {
        NonnullRefPtr<SkiaBackendContext> nonnull_context = context.release_value();
        auto duplicated_payload = duplicate_linux_dmabuf_payload(payload, payload.color_space());
        auto shared_image = SharedImage::import_from_payload(move(duplicated_payload));
        auto painting_surface = shared_image.create_painting_surface(nonnull_context, PaintingSurface::Origin::TopLeft);
        TRY(upload_decoded_image_frame_to_surface(frame, painting_surface));
        return {};
    }

    auto bitmap_format = payload.info().pixel_format;
    if (bitmap_format != BitmapFormat::BGRA8888
        && bitmap_format != BitmapFormat::BGRx8888
        && bitmap_format != BitmapFormat::RGBA8888
        && bitmap_format != BitmapFormat::RGBx8888) {
        return Error::from_string_literal("VulkanImage: unsupported_cpu_bitmap_format");
    }

    int fd = dmabuf->file.fd();
    if (fd < 0)
        return Error::from_string_literal("VulkanImage: invalid_dma_buf_fd");

    size_t stride = dmabuf->stride;
    size_t offset = dmabuf->offset;
    if (stride == 0)
        return Error::from_string_literal("VulkanImage: zero_dma_buf_stride");

    auto const& info = payload.info();
    int width = info.size.width();
    int height = info.size.height();
    if (width == 0 || height == 0)
        return Error::from_string_literal("VulkanImage: invalid_payload_dimensions");
    if (width != frame.width() || height != frame.height())
        return Error::from_string_literal("VulkanImage: payload_size_mismatch");

    size_t bytes_per_row = static_cast<size_t>(width) * 4;
    if (bytes_per_row > stride)
        return Error::from_string_literal("VulkanImage: payload_stride_too_small");
    if (Checked<size_t>::multiplication_would_overflow(stride, height))
        return Error::from_string_literal("VulkanImage: payload_stride_overflow");
    size_t required_size = offset + (stride * height);

    void* mapping = mmap(nullptr, required_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return Error::from_string_literal("VulkanImage: mmap_failed");
    ArmedScopeGuard unmap_on_return([&] {
        munmap(mapping, required_size);
    });

    if (!dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
        return Error::from_string_literal("VulkanImage: dma_buf_sync_start_failed");

    auto destination_bitmap = TRY(Bitmap::create_wrapper(bitmap_format, info.alpha_type, info.size, stride, static_cast<u8*>(mapping) + offset));
    TRY(upload_decoded_image_frame_to_bitmap(frame, *destination_bitmap, info, payload.color_space()));

    if (!dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
        return Error::from_string_literal("VulkanImage: dma_buf_sync_end_failed");

    return {};
}

static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf(BitmapInfo const& info, LinuxDmaBufPayload const& dmabuf)
{
    VERIFY(info.pixel_format == BitmapFormat::BGRA8888 || info.pixel_format == BitmapFormat::RGBA8888);
    auto data_size = Bitmap::size_in_bytes(dmabuf.stride, info.size.height());
    auto* data = ::mmap(nullptr, data_size, PROT_READ, MAP_SHARED, dmabuf.file.fd(), 0);
    VERIFY(data != MAP_FAILED);

    return MUST(Bitmap::create_wrapper(info.pixel_format, info.alpha_type, info.size, dmabuf.stride, data, [data, data_size] {
        VERIFY(::munmap(data, data_size) == 0);
    }));
}

static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf_payload(SharedImagePayload const& payload)
{
    auto const* dmabuf = payload.linux_dma_buf_payload();
    VERIFY(dmabuf);
    return create_bitmap_from_linux_dmabuf(payload.info(), *dmabuf);
}

static ErrorOr<NonnullRefPtr<SkiaBackendContext>> shared_image_skia_backend_context()
{
    static RefPtr<SkiaBackendContext> context;
    if (context)
        return *context;

    auto vulkan_context = TRY(create_vulkan_context());
    context = SkiaBackendContext::create_vulkan_context(vulkan_context);
    if (!context)
        return Error::from_string_literal("VulkanImage: failed_to_create_skia_backend_context");

    return *context;
}

static ErrorOr<void> upload_decoded_image_frame_to_surface(DecodedImageFrame const& frame, PaintingSurface& painting_surface)
{
    auto sk_image = sk_image_from_bitmap(frame.bitmap(), frame.color_space());
    if (!sk_image)
        return Error::from_string_literal("VulkanImage: missing_source_image");

    painting_surface.lock_context();
    auto& canvas = painting_surface.canvas();
    canvas.clear(SK_ColorTRANSPARENT);
    canvas.drawImage(sk_image, 0.0f, 0.0f);
    painting_surface.unlock_context();

    if (auto surface_context = painting_surface.skia_backend_context()) {
        painting_surface.lock_context();
        surface_context->flush_and_submit(&painting_surface.sk_surface());
        painting_surface.unlock_context();
    }

    return {};
}

static LinuxDmaBufPayload create_linux_dmabuf_payload(VulkanImage const& vulkan_image, BitmapInfo const& info)
{
    VERIFY(vulkan_image.info.modifier == DRM_FORMAT_MOD_LINEAR);
    auto fd = vulkan_image.get_dma_buf_fd();
    VERIFY(fd >= 0);

    VkFormat drm_format;
    if (info.pixel_format == BitmapFormat::BGRA8888)
        drm_format = VK_FORMAT_B8G8R8A8_UNORM;
    else if (info.pixel_format == BitmapFormat::RGBA8888)
        drm_format = VK_FORMAT_R8G8B8A8_UNORM;
    else if (info.pixel_format == BitmapFormat::RGBAF16)
        drm_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    else
        VERIFY_NOT_REACHED();

    return LinuxDmaBufPayload {
        .drm_format = drm_format,
        .stride = static_cast<u32>(vulkan_image.info.row_pitch),
        .offset = 0,
        .file = IPC::File::adopt_fd(fd),
    };
}

static bool dmabuf_sync(int fd, unsigned flags)
{
    dma_buf_sync sync {
        .flags = flags,
    };

    int rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (rc == 0)
        return true;

    if (errno == ENOTTY || errno == EINVAL)
        return true;

    return false;
}

}

#endif
