/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/MetalContext.h>

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

namespace Gfx {

class MetalTextureImpl final : public MetalTexture {
public:
    MetalTextureImpl(id<MTLTexture> texture)
        : m_texture(texture)
    {
    }

    void const* texture() const override { return m_texture; }
    size_t width() const override { return m_texture.width; }
    size_t height() const override { return m_texture.height; }

    virtual ~MetalTextureImpl()
    {
        [m_texture release];
    }

private:
    id<MTLTexture> m_texture;
};

class MetalContextImpl final : public MetalContext {
public:
    MetalContextImpl(id<MTLDevice> device, id<MTLCommandQueue> queue)
        : m_device(device)
        , m_queue(queue)
    {
    }

    void const* device() const override { return m_device; }
    void const* queue() const override { return m_queue; }

    OwnPtr<MetalTexture> create_texture_from_iosurface(void* platform_surface_handle, IntSize size, BitmapFormat bitmap_format) override
    {
        auto* const descriptor = [[MTLTextureDescriptor alloc] init];
        switch (bitmap_format) {
        case BitmapFormat::BGRA8888:
        case BitmapFormat::BGRx8888:
            descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
            break;
        case BitmapFormat::RGBA8888:
        case BitmapFormat::RGBx8888:
            descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        descriptor.width = size.width();
        descriptor.height = size.height();
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

        id<MTLTexture> texture = [m_device newTextureWithDescriptor:descriptor iosurface:(IOSurfaceRef)platform_surface_handle plane:0];
        [descriptor release];
        return make<MetalTextureImpl>(texture);
    }

    virtual ~MetalContextImpl() override
    {
        [m_queue release];
        [m_device release];
    }

private:
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_queue;
};

RefPtr<MetalContext> get_metal_context()
{
    auto device = MTLCreateSystemDefaultDevice();
    if (!device) {
        dbgln("Failed to create Metal device");
        return {};
    }

    auto queue = [device newCommandQueue];
    if (!queue) {
        dbgln("Failed to create Metal command queue");
        [device release];
        return {};
    }

    return adopt_ref(*new MetalContextImpl(device, queue));
}

}
