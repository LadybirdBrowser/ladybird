/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/RenderPassEncoder.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct RenderPassEncoder::Impl {
    explicit Impl(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const&);

    ~Impl();

    ErrorOr<void> initialize();

    VkRenderPass render_pass() const { return m_render_pass; }

    RenderPassDescriptor const& render_pass_descriptor() const { return m_render_pass_descriptor; }

    void end();

private:
    VkDevice m_logical_device = { VK_NULL_HANDLE };
    VkCommandBuffer m_command_buffer = { VK_NULL_HANDLE };

    RenderPassDescriptor m_render_pass_descriptor;
    VkRenderPass m_render_pass = { VK_NULL_HANDLE };
};

}
