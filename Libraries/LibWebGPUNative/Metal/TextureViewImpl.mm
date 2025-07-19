/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/Error.h>
#include <LibWebGPUNative/Metal/TextureImpl.h>
#include <LibWebGPUNative/Metal/TextureViewImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

TextureView::Impl::Impl(Texture const& gpu_texture)
    : m_size(gpu_texture.size())
{
    id texture = gpu_texture.m_impl->texture();
    if (texture) {
        m_texture = make<MetalTextureHandle>(texture);
    }
}

ErrorOr<void> TextureView::Impl::initialize()
{
    if (!m_texture) {
        return make_error("No texture available");
    }

    id<MTLTexture> texture = static_cast<id<MTLTexture>>(m_texture->get());
    // TODO: Support configurable pixel format via WebGPU GPUTextureViewDescriptor
    id<MTLTexture> texture_view = [texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
    if (!texture_view) {
        return make_error("Unable to create texture view");
    }

    m_texture_view = make<MetalTextureHandle>(texture_view);

    return {};
}

}
