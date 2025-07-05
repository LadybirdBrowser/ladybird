/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/CommandEncoderImpl.h>
#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/RenderPassEncoderImpl.h>

namespace WebGPUNative {

RenderPassEncoder::Impl::Impl(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const& gpu_render_pass_descriptor)
    : m_logical_device(gpu_command_encoder.m_impl->logical_device())
    , m_command_buffer(gpu_command_encoder.m_impl->command_buffer())
    , m_render_pass_descriptor(gpu_render_pass_descriptor)
{
}

RenderPassEncoder::Impl::~Impl()
{
    vkDestroyRenderPass(m_logical_device, m_render_pass, nullptr);
}

ErrorOr<void> RenderPassEncoder::Impl::initialize()
{
    // FIXME: Don't hardcode these settings
    VkAttachmentDescription attachment_description = {};
    attachment_description.format = VK_FORMAT_R8G8B8A8_SRGB;
    attachment_description.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment_description.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment_description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // FIXME: Add depth stencil support
    attachment_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment_description.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment_description.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference attachment_reference = {};
    attachment_reference.attachment = 0;
    attachment_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass_description = {};
    subpass_description.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass_description.colorAttachmentCount = 1;
    subpass_description.pColorAttachments = &attachment_reference;

    VkRenderPassCreateInfo render_pass_create_info = {};
    render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_create_info.attachmentCount = 1;
    render_pass_create_info.pAttachments = &attachment_description;
    render_pass_create_info.subpassCount = 1;
    render_pass_create_info.pSubpasses = &subpass_description;

    if (VkResult const create_render_pass_result = vkCreateRenderPass(m_logical_device, &render_pass_create_info, nullptr, &m_render_pass); create_render_pass_result != VK_SUCCESS)
        return make_error(create_render_pass_result, "Unable to create render pass");
    return {};
}

void RenderPassEncoder::Impl::end()
{
    vkCmdEndRenderPass(m_command_buffer);
}

}
