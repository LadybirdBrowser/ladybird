/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/TextureImpl.h>
#include <LibWebGPUNative/Vulkan/TextureViewImpl.h>

namespace WebGPUNative {

TextureView::Impl::Impl(Texture const& gpu_texture)
    : m_size(gpu_texture.size())
    , m_device(gpu_texture.m_impl->device())
    , m_image(gpu_texture.m_impl->image())
{
}

TextureView::Impl::~Impl()
{
    vkDestroyImageView(m_device, m_image_view, nullptr);
}

ErrorOr<void> TextureView::Impl::initialize()
{
    VkImageViewCreateInfo create_image_view_info = {};
    create_image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    create_image_view_info.image = m_image;
    // FIXME: Don't hardcode these settings
    create_image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    create_image_view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    create_image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    create_image_view_info.subresourceRange.baseMipLevel = 0;
    create_image_view_info.subresourceRange.levelCount = 1;
    create_image_view_info.subresourceRange.baseArrayLayer = 0;
    create_image_view_info.subresourceRange.layerCount = 1;

    if (VkResult const create_image_view_result = vkCreateImageView(m_device, &create_image_view_info, nullptr, &m_image_view); create_image_view_result != VK_SUCCESS)
        return make_error(create_image_view_result, "Unable to create image view");
    return {};
}

}
