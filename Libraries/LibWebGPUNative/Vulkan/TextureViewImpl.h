/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Size.h>
#include <LibWebGPUNative/TextureView.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct TextureView::Impl {
    explicit Impl(Texture const& gpu_texture);

    ~Impl();

    ErrorOr<void> initialize();

    Gfx::IntSize size() const { return m_size; }

    VkImageView image_view() const { return m_image_view; }

private:
    Gfx::IntSize m_size;
    VkDevice m_device = { VK_NULL_HANDLE };
    VkImage m_image = { VK_NULL_HANDLE };
    VkImageView m_image_view = { VK_NULL_HANDLE };
};

}
