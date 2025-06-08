/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibWebGPUNative/Vulkan/DeviceImpl.h>
#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/TextureImpl.h>

namespace WebGPUNative {

Texture::Impl::Impl(Device const& gpu_device, Gfx::IntSize const size)
    : m_size(size)
    , m_physical_device(gpu_device.m_impl->physical_device())
    , m_logical_device(gpu_device.m_impl->logical_device())
    , m_queue(gpu_device.m_impl->queue())
    , m_command_pool(gpu_device.m_impl->command_pool())
{
}

Texture::Impl::~Impl()
{
    vkFreeMemory(m_logical_device, m_image_memory, nullptr);
    vkDestroyImage(m_logical_device, m_image, nullptr);
}

ErrorOr<void> Texture::Impl::initialize()
{
    VkImageCreateInfo create_image_info = {};
    create_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // FIXME: Don't hardcode these settings
    create_image_info.imageType = VK_IMAGE_TYPE_2D;
    create_image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    create_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // https://www.w3.org/TR/webgpu/#typedefdef-gputextureusageflags
    create_image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create_image_info.arrayLayers = 1;
    create_image_info.mipLevels = 1;
    create_image_info.extent.depth = 1;
    create_image_info.extent.height = static_cast<uint32_t>(m_size.height());
    create_image_info.extent.width = static_cast<uint32_t>(m_size.width());

    if (VkResult const create_image_result = vkCreateImage(m_logical_device, &create_image_info, nullptr, &m_image); create_image_result != VK_SUCCESS)
        return make_error(create_image_result, "Unable to create image");

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(m_logical_device, m_image, &memory_requirements);

    Optional<uint32_t> memory_type_index;

    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memory_properties);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((memory_requirements.memoryTypeBits & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memory_type_index = i;
            break;
        }
    }

    if (!memory_type_index.has_value())
        return make_error("No supported physical device memory available");

    VkMemoryAllocateInfo memory_allocate_info = {};
    memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_allocate_info.allocationSize = memory_requirements.size;
    memory_allocate_info.memoryTypeIndex = memory_type_index.value();

    if (VkResult const allocate_memory_result = vkAllocateMemory(m_logical_device, &memory_allocate_info, nullptr, &m_image_memory); allocate_memory_result != VK_SUCCESS)
        return make_error(allocate_memory_result, "Unable to allocate memory");
    if (VkResult const bind_image_memory_result = vkBindImageMemory(m_logical_device, m_image, m_image_memory, 0); bind_image_memory_result != VK_SUCCESS)
        return make_error(bind_image_memory_result, "Unable to bind image memory");
    return {};
}

ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> Texture::Impl::map_buffer()
{
    size_t const buffer_size = m_size.width() * m_size.height() * 4; // RGBA

    VkBufferCreateInfo buffer_create_info = {};
    buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_create_info.size = buffer_size;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (VkResult const create_buffer_Result = vkCreateBuffer(m_logical_device, &buffer_create_info, nullptr, &m_drawing_buffer); create_buffer_Result != VK_SUCCESS)
        return make_error(create_buffer_Result, "Unable to create buffer");

    VkMemoryRequirements memory_requirements;
    vkGetBufferMemoryRequirements(m_logical_device, m_drawing_buffer, &memory_requirements);

    Optional<uint32_t> memory_type_index;

    VkPhysicalDeviceMemoryProperties memory_properties = {};
    constexpr VkMemoryPropertyFlags memory_property_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memory_properties);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((memory_requirements.memoryTypeBits & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & memory_property_flags) == memory_property_flags) {
            memory_type_index = i;
            break;
        }
    }

    if (!memory_type_index.has_value())
        return make_error("No supported physical device memory available");

    VkMemoryAllocateInfo memory_allocate_info = {};
    memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_allocate_info.allocationSize = memory_requirements.size;
    memory_allocate_info.memoryTypeIndex = memory_type_index.value();

    if (VkResult const allocate_memory_result = vkAllocateMemory(m_logical_device, &memory_allocate_info, nullptr, &m_drawing_buffer_memory); allocate_memory_result != VK_SUCCESS)
        return make_error(allocate_memory_result, "Unable to allocate memory");

    if (VkResult const bind_buffer_memory_result = vkBindBufferMemory(m_logical_device, m_drawing_buffer, m_drawing_buffer_memory, 0); bind_buffer_memory_result != VK_SUCCESS)
        return make_error(bind_buffer_memory_result, "Unable to bind buffer memory");

    VkCommandBufferAllocateInfo command_buffer_allocate_info = {};
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandPool = m_command_pool;
    command_buffer_allocate_info.commandBufferCount = 1;

    if (VkResult const allocate_command_buffers_result = vkAllocateCommandBuffers(m_logical_device, &command_buffer_allocate_info, &m_command_buffer); allocate_command_buffers_result != VK_SUCCESS)
        return make_error(allocate_command_buffers_result, "Unable to allocate command buffers");

    VkCommandBufferBeginInfo command_buffer_begin_info = {};
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (VkResult const begin_command_buffer_result = vkBeginCommandBuffer(m_command_buffer, &command_buffer_begin_info); begin_command_buffer_result != VK_SUCCESS)
        return make_error(begin_command_buffer_result, "Unable to begin command buffer");

    VkImageMemoryBarrier image_memory_transfer_src_barrier = {};
    image_memory_transfer_src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_memory_transfer_src_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_memory_transfer_src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_memory_transfer_src_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_memory_transfer_src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_memory_transfer_src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_memory_transfer_src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_memory_transfer_src_barrier.image = m_image;
    image_memory_transfer_src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_memory_transfer_src_barrier.subresourceRange.baseMipLevel = 0;
    image_memory_transfer_src_barrier.subresourceRange.levelCount = 1;
    image_memory_transfer_src_barrier.subresourceRange.baseArrayLayer = 0;
    image_memory_transfer_src_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_memory_transfer_src_barrier);

    VkBufferImageCopy buffer_image_copy = {};
    buffer_image_copy.bufferOffset = 0;
    buffer_image_copy.bufferRowLength = 0;
    buffer_image_copy.bufferImageHeight = 0;
    buffer_image_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    buffer_image_copy.imageSubresource.mipLevel = 0;
    buffer_image_copy.imageSubresource.baseArrayLayer = 0;
    buffer_image_copy.imageSubresource.layerCount = 1;
    buffer_image_copy.imageOffset = { 0, 0, 0 };
    buffer_image_copy.imageExtent = { static_cast<uint32_t>(m_size.width()), static_cast<uint32_t>(m_size.height()), 1 };

    vkCmdCopyImageToBuffer(m_command_buffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_drawing_buffer, 1, &buffer_image_copy);

    VkImageMemoryBarrier image_memory_color_attachment_barrier = {};
    image_memory_color_attachment_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_memory_color_attachment_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_memory_color_attachment_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_memory_color_attachment_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_memory_color_attachment_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_memory_color_attachment_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_memory_color_attachment_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_memory_color_attachment_barrier.image = m_image;
    image_memory_color_attachment_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_memory_color_attachment_barrier.subresourceRange.baseMipLevel = 0;
    image_memory_color_attachment_barrier.subresourceRange.levelCount = 1;
    image_memory_color_attachment_barrier.subresourceRange.baseArrayLayer = 0;
    image_memory_color_attachment_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_memory_color_attachment_barrier);

    if (VkResult const end_command_buffer_result = vkEndCommandBuffer(m_command_buffer); end_command_buffer_result != VK_SUCCESS)
        return make_error(end_command_buffer_result, "Unable to end command buffer");

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_command_buffer;

    // FIXME: Queue submission should be asynchronous
    if (VkResult const queue_submit_result = vkQueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE); queue_submit_result != VK_SUCCESS)
        return make_error(queue_submit_result, "Unable to submit command buffer to queue");

    if (VkResult const queue_wait_idle_result = vkQueueWaitIdle(m_queue); queue_wait_idle_result != VK_SUCCESS)
        return make_error(queue_wait_idle_result, "Unable to wait for queue to be idle");

    vkFreeCommandBuffers(m_logical_device, m_command_pool, 1, &m_command_buffer);
    m_command_buffer = VK_NULL_HANDLE;

    void* mapped_buffer = nullptr;
    if (VkResult const map_memory_result = vkMapMemory(m_logical_device, m_drawing_buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped_buffer); map_memory_result != VK_SUCCESS)
        return make_error(map_memory_result, "Unable to map memory");
    // NOTE: ErrorOr disable RVO, so we use reference semantics here to avoid the destructive move that results in unmapping the buffer twice
    return make<MappedTextureBuffer>(*this, static_cast<u8*>(mapped_buffer), buffer_size, m_size.width());
}

void Texture::Impl::unmap_buffer()
{
    vkUnmapMemory(m_logical_device, m_drawing_buffer_memory);
    vkFreeMemory(m_logical_device, m_drawing_buffer_memory, nullptr);
    vkDestroyBuffer(m_logical_device, m_drawing_buffer, nullptr);
}

MappedTextureBuffer::MappedTextureBuffer(Texture::Impl& texture_impl, u8* buffer, size_t buffer_size, u32 row_pitch)
    : m_texture_impl(texture_impl)
    , m_buffer(buffer, buffer_size)
    , m_row_pitch(row_pitch)
{
}

MappedTextureBuffer::~MappedTextureBuffer()
{
    m_texture_impl->unmap_buffer();
}

int MappedTextureBuffer::width() const
{
    return m_texture_impl->size().width();
}

int MappedTextureBuffer::height() const
{
    return m_texture_impl->size().height();
}

}
