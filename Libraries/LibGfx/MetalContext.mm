/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/MetalContext.h>

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

static MTLPixelFormat metal_pixel_format(MetalTextureFormat format)
{
    switch (format) {
    case MetalTextureFormat::BGRA8:
        return MTLPixelFormatBGRA8Unorm;
    case MetalTextureFormat::R8:
        return MTLPixelFormatR8Unorm;
    case MetalTextureFormat::RG8:
        return MTLPixelFormatRG8Unorm;
    case MetalTextureFormat::R16:
        return MTLPixelFormatR16Unorm;
    case MetalTextureFormat::RG16:
        return MTLPixelFormatRG16Unorm;
    }
    VERIFY_NOT_REACHED();
}

class MetalContextImpl final : public MetalContext {
public:
    MetalContextImpl(id<MTLDevice> device, id<MTLCommandQueue> queue)
        : m_device(device)
        , m_queue(queue)
    {
    }

    void const* device() const override { return m_device; }
    void const* queue() const override { return m_queue; }

    OwnPtr<MetalTexture> create_texture_from_iosurface(Core::IOSurfaceHandle const& iosurface, MetalTextureFormat format, size_t plane) override
    {
        auto* const descriptor = [[MTLTextureDescriptor alloc] init];
        descriptor.pixelFormat = metal_pixel_format(format);
        descriptor.width = iosurface.plane_count() > 0 ? iosurface.plane_width(plane) : iosurface.width();
        descriptor.height = iosurface.plane_count() > 0 ? iosurface.plane_height(plane) : iosurface.height();
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

        id<MTLTexture> texture = [m_device newTextureWithDescriptor:descriptor iosurface:(IOSurfaceRef)iosurface.core_foundation_pointer() plane:plane];
        [descriptor release];
        if (!texture)
            return {};
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
