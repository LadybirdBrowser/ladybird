/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/CommandEncoderImpl.h>
#include <LibWebGPUNative/Vulkan/DeviceImpl.h>
#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/RenderPassEncoderImpl.h>
#include <LibWebGPUNative/Vulkan/TextureViewImpl.h>

namespace WebGPUNative {

CommandEncoder::Impl::Impl(Device const& gpu_device)
    : m_logical_device(gpu_device.m_impl->logical_device())
    , m_command_pool(gpu_device.m_impl->command_pool())
{
}

CommandEncoder::Impl::~Impl()
{
    // FIXME: Should move this into GPURenderPassEncoder dtor instead
    vkDestroyFramebuffer(m_logical_device, m_frame_buffer, nullptr);
    vkFreeCommandBuffers(m_logical_device, m_command_pool, 1, &m_command_buffer);
}

ErrorOr<void> CommandEncoder::Impl::initialize()
{
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
    return {};
}

ErrorOr<void> CommandEncoder::Impl::begin_render_pass(RenderPassEncoder const& render_pass_encoder)
{
    // FIXME: Don't hardcode these settings

    RenderPassDescriptor const& render_pass_descriptor = render_pass_encoder.render_pass_descriptor();

    VkFramebufferCreateInfo framebuffer_create_info = {};
    framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_create_info.renderPass = render_pass_encoder.m_impl->render_pass();

    // FIXME: Should we get this from the GPUCanvasContext? All views should have the same size given they are made from the GPUTexture owned by the canvas
    Gfx::IntSize extent;

    auto const attachment_count = render_pass_descriptor.color_attachments.size();
    Vector<VkImageView> views;
    Vector<VkClearValue> clear_values;
    for (auto const& color_attachment : render_pass_descriptor.color_attachments) {
        auto texture_view = color_attachment.view;
        views.append(texture_view->m_impl->image_view());

        if (color_attachment.clear_value.has_value()) {
            auto const& [r, g, b, a] = color_attachment.clear_value.value();
            VkClearColorValue const clear_color_value = {
                .float32 = { static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a) }
            };
            VkClearValue clear_value = {
                .color = clear_color_value
            };
            clear_values.append(clear_value);
        }

        // FIXME: Get the extent properly from the single GPUTexture we are rendering into
        extent = texture_view->m_impl->size();
    }
    framebuffer_create_info.attachmentCount = static_cast<uint32_t>(attachment_count);
    framebuffer_create_info.pAttachments = views.data();
    framebuffer_create_info.width = extent.width();
    framebuffer_create_info.height = extent.height();
    framebuffer_create_info.layers = 1;
    // FIXME: Should this be created in GPURenderPassEncoder instead? As its lifetime needs to outlast finish()
    if (VkResult const create_frame_buffer_result = vkCreateFramebuffer(m_logical_device, &framebuffer_create_info, nullptr, &m_frame_buffer); create_frame_buffer_result != VK_SUCCESS)
        return make_error(create_frame_buffer_result, "Unable to create frame buffer");

    VkRenderPassBeginInfo render_pass_begin_info = {};
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.renderPass = render_pass_encoder.m_impl->render_pass();
    render_pass_begin_info.framebuffer = m_frame_buffer;
    render_pass_begin_info.renderArea.offset = { 0, 0 };
    render_pass_begin_info.renderArea.extent = { .width = static_cast<uint32_t>(extent.width()), .height = static_cast<uint32_t>(extent.height()) };
    if (!clear_values.is_empty()) {
        render_pass_begin_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
        render_pass_begin_info.pClearValues = clear_values.data();
    }
    vkCmdBeginRenderPass(m_command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    return {};
}

ErrorOr<void> CommandEncoder::Impl::finish()
{
    if (VkResult const end_command_buffer = vkEndCommandBuffer(m_command_buffer); end_command_buffer != VK_SUCCESS)
        return make_error(end_command_buffer, "Unable to end command buffer");
    return {};
}

}
