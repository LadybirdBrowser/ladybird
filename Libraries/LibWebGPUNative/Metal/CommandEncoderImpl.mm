/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-License: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/CommandEncoderImpl.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#include <LibWebGPUNative/Metal/RenderPassEncoderImpl.h>
#include <LibWebGPUNative/Metal/TextureViewImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

CommandEncoder::Impl::Impl(Device const& gpu_device)
{
    id command_queue = gpu_device.m_impl->mtl_command_queue();
    if (command_queue) {
        m_command_queue = make<MetalCommandQueueHandle>(command_queue);
    }
}

ErrorOr<void> CommandEncoder::Impl::initialize()
{
    if (!m_command_queue) {
        return make_error("No command queue available");
    }

    id<MTLCommandQueue> command_queue = static_cast<id<MTLCommandQueue>>(m_command_queue->get());
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (!command_buffer) {
        return make_error("Unable to create command buffer");
    }

    m_command_buffer = make<MetalCommandBufferHandle>(command_buffer);

    // Metal command buffers are ready to encode upon creation; no explicit begin is needed.

    return {};
}

ErrorOr<void> CommandEncoder::Impl::begin_render_pass(RenderPassEncoder const& render_pass_encoder)
{
    if (!m_command_buffer) {
        return make_error("No command buffer available");
    }

    RenderPassDescriptor const& render_pass_descriptor = render_pass_encoder.render_pass_descriptor();

    // TODO: Support configurable settings via WebGPU RenderPassDescriptor
    MTLRenderPassDescriptor* metal_render_pass_descriptor = [[MTLRenderPassDescriptor alloc] init];

    // TODO: Get extent properly from the GPUTexture
    Gfx::IntSize extent;

    size_t attachment_index = 0;
    for (auto const& color_attachment : render_pass_descriptor.color_attachments) {
        if (attachment_index >= 8) { // MTLRenderPassDescriptor supports up to 8 color attachments
            [metal_render_pass_descriptor release];
            return make_error("Too many color attachments");
        }

        auto texture_view = color_attachment.view;
        metal_render_pass_descriptor.colorAttachments[attachment_index].texture = static_cast<id<MTLTexture>>(texture_view->m_impl->texture_view());
        metal_render_pass_descriptor.colorAttachments[attachment_index].loadAction = MTLLoadActionClear;
        metal_render_pass_descriptor.colorAttachments[attachment_index].storeAction = MTLStoreActionStore;

        if (color_attachment.clear_value.has_value()) {
            auto const& [r, g, b, a] = color_attachment.clear_value.value();
            metal_render_pass_descriptor.colorAttachments[attachment_index].clearColor = MTLClearColorMake(r, g, b, a);
        } else {
            metal_render_pass_descriptor.colorAttachments[attachment_index].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        }

        // TODO: Get extent properly from the single GPUTexture
        extent = texture_view->m_impl->size();
        ++attachment_index;
    }

    if (attachment_index == 0) {
        [metal_render_pass_descriptor release];
        return make_error("No color attachments specified");
    }

    id<MTLCommandBuffer> command_buffer = static_cast<id<MTLCommandBuffer>>(m_command_buffer->get());
    id<MTLRenderCommandEncoder> render_command_encoder = [command_buffer renderCommandEncoderWithDescriptor:metal_render_pass_descriptor];
    [metal_render_pass_descriptor release];

    if (!render_command_encoder) {
        return make_error("Unable to create render command encoder");
    }

    m_render_command_encoder = make<MetalRenderCommandEncoderHandle>(render_command_encoder);

    return {};
}

ErrorOr<void> CommandEncoder::Impl::finish()
{
    if (!m_command_buffer) {
        return make_error("No command buffer available");
    }

    if (m_render_command_encoder) {
        [static_cast<id<MTLRenderCommandEncoder>>(m_render_command_encoder->get()) endEncoding];
        m_render_command_encoder = nullptr;
    }

    // Metal command buffers are implicitly ended when committed; no explicit end is needed.

    return {};
}

}
