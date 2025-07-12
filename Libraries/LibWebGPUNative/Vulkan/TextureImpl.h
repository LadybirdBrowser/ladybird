/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Texture.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct Texture::Impl {
    explicit Impl(Device const& gpu_device, Gfx::IntSize size);

    ~Impl();

    ErrorOr<void> initialize();

    Gfx::IntSize size() const { return m_size; }

    VkDevice device() const { return m_logical_device; }

    VkImage image() const { return m_image; }

    ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> map_buffer();
    void unmap_buffer();

private:
    Gfx::IntSize m_size;
    VkPhysicalDevice m_physical_device = { VK_NULL_HANDLE };
    VkDevice m_logical_device = { VK_NULL_HANDLE };
    VkQueue m_queue = { VK_NULL_HANDLE };
    VkCommandPool m_command_pool = { VK_NULL_HANDLE };

    VkCommandBuffer m_command_buffer = { VK_NULL_HANDLE };

    VkImage m_image = { VK_NULL_HANDLE };
    VkDeviceMemory m_image_memory = { VK_NULL_HANDLE };

    VkBuffer m_drawing_buffer = { VK_NULL_HANDLE };
    VkDeviceMemory m_drawing_buffer_memory = { VK_NULL_HANDLE };
};

}
