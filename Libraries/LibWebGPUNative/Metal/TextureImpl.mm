/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Span.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#include <LibWebGPUNative/Metal/TextureImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

Texture::Impl::Impl(Device const& gpu_device, Gfx::IntSize size)
    : m_size(size)
{
    id command_queue = gpu_device.m_impl->mtl_command_queue();
    if (command_queue) {
        m_command_queue = make<MetalCommandQueueHandle>(command_queue);
    }
}

ErrorOr<void> Texture::Impl::initialize()
{
    MTLTextureDescriptor* texture_descriptor = [[MTLTextureDescriptor alloc] init];
    texture_descriptor.textureType = MTLTextureType2D;
    texture_descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm_sRGB;
    texture_descriptor.width = static_cast<NSUInteger>(m_size.width());
    texture_descriptor.height = static_cast<NSUInteger>(m_size.height());
    texture_descriptor.mipmapLevelCount = 1;
    texture_descriptor.sampleCount = 1;
    texture_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    // FIXME: Keep this private/managed and use proper synchronization here to get host access to the texture data
    texture_descriptor.storageMode = MTLStorageModeShared;

    id<MTLCommandQueue> command_queue = static_cast<id<MTLCommandQueue>>(m_command_queue->get());
    id<MTLDevice> device = command_queue.device;
    id<MTLTexture> texture = [device newTextureWithDescriptor:texture_descriptor];
    [texture_descriptor release];

    if (!texture) {
        return make_error("Unable to create texture");
    }

    m_texture = make<MetalTextureHandle>(texture);

    return {};
}

ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> Texture::Impl::map_buffer()
{
    if (!m_texture) {
        return make_error("No texture available");
    }

    id<MTLTexture> texture = static_cast<id<MTLTexture>>(m_texture->get());
    size_t buffer_size = m_size.width() * m_size.height() * 4; // RGBA

    void* mapped_buffer = malloc(buffer_size);
    if (!mapped_buffer) {
        return make_error("Unable to allocate memory for texture data");
    }

    [texture getBytes:mapped_buffer
          bytesPerRow:m_size.width() * 4
           fromRegion:MTLRegionMake2D(0, 0, m_size.width(), m_size.height())
          mipmapLevel:0];

    return make<MappedTextureBuffer>(*this, static_cast<u8*>(mapped_buffer), buffer_size, m_size.width());
}

void Texture::Impl::unmap_buffer()
{
}

MappedTextureBuffer::MappedTextureBuffer(Texture::Impl& texture_impl, u8* buffer, size_t buffer_size, u32 row_pitch)
    : m_texture_impl(texture_impl)
    , m_buffer(buffer, buffer_size)
    , m_row_pitch(row_pitch)
{
}

MappedTextureBuffer::~MappedTextureBuffer()
{
    if (m_buffer.data()) {
        free(m_buffer.data());
    }
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
