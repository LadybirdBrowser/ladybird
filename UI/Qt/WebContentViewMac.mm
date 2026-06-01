/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Math.h>
#include <LibGfx/SharedImageBuffer.h>
#include <UI/Qt/WebContentView.h>

#include <QColor>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

namespace Ladybird {

static void release_metal_object(void*& object)
{
    if (!object)
        return;

    [(id)object release];
    object = nullptr;
}

void WebContentView::release_imported_iosurface_texture()
{
    release_metal_object(m_imported_iosurface_texture);
    m_imported_shared_image_buffer = nullptr;
}

void WebContentView::release_metal_resources()
{
    release_imported_iosurface_texture();
    release_metal_object(m_metal_sampler_state);
    release_metal_object(m_metal_pipeline_state);
    release_metal_object(m_metal_library);
    m_metal_device = nullptr;
    m_render_target_pixel_format = 0;
}

bool WebContentView::prepare_metal_renderer(unsigned long render_target_pixel_format)
{
    auto const* rhi_native_handles = static_cast<QRhiMetalNativeHandles const*>(rhi()->nativeHandles());
    if (!rhi_native_handles || !rhi_native_handles->dev)
        return false;

    auto* device = (id<MTLDevice>)rhi_native_handles->dev;
    if (m_metal_device != device || m_render_target_pixel_format != render_target_pixel_format) {
        release_metal_resources();
        m_metal_device = device;
        m_render_target_pixel_format = render_target_pixel_format;
    }

    if (m_metal_pipeline_state && m_metal_sampler_state)
        return true;

    // NB: QRhiWidget gives us a Metal render target for the widget, but QRhi does
    // not expose a way to wrap the IOSurface-backed texture from WebContent as a
    // QRhiTexture. Importing the IOSurface with Metal and drawing a textured quad
    // lets us present it without doing a CPU readback through QImage/QPainter.
    //
    // The shader deliberately keeps the geometry tiny: it maps the painted content
    // rectangle into the QRhiWidget render target and samples the corresponding
    // sub-rectangle from the IOSurface. The IOSurface is imported as BGRA below;
    // Metal texture sampling exposes those pixels to the shader as logical RGBA
    // components, so the fragment shader does not need an explicit swizzle.
    auto const* shader_source = R"(
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float2 target_size;
    float2 content_size;
    float2 source_size;
};

struct VertexOut {
    float4 position [[position]];
    float2 texture_coordinate;
};

vertex VertexOut vertex_main(uint vertex_id [[vertex_id]], constant Uniforms& uniforms [[buffer(0)]])
{
    float2 unit_positions[4] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 1.0),
    };

    float2 unit_position = unit_positions[vertex_id];
    float2 pixel_position = unit_position * uniforms.content_size;

    VertexOut out;
    out.position = float4(
        pixel_position.x / uniforms.target_size.x * 2.0 - 1.0,
        1.0 - pixel_position.y / uniforms.target_size.y * 2.0,
        0.0,
        1.0);
    out.texture_coordinate = pixel_position / uniforms.source_size;
    return out;
}

fragment half4 fragment_main(VertexOut in [[stage_in]], texture2d<half> texture [[texture(0)]], sampler texture_sampler [[sampler(0)]])
{
    return texture.sample(texture_sampler, in.texture_coordinate);
}
)";

    NSError* error = nil;
    auto* library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:nil error:&error];
    if (!library) {
        dbgln("Failed to create Metal shader library for Qt WebContentView");
        return false;
    }
    m_metal_library = library;

    auto* vertex_function = [library newFunctionWithName:@"vertex_main"];
    auto* fragment_function = [library newFunctionWithName:@"fragment_main"];
    if (!vertex_function || !fragment_function) {
        [vertex_function release];
        [fragment_function release];
        dbgln("Failed to create Metal shader functions for Qt WebContentView");
        return false;
    }

    auto* pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.vertexFunction = vertex_function;
    pipeline_descriptor.fragmentFunction = fragment_function;
    pipeline_descriptor.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(render_target_pixel_format);

    auto* pipeline_state = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
    [pipeline_descriptor release];
    [vertex_function release];
    [fragment_function release];

    if (!pipeline_state) {
        dbgln("Failed to create Metal render pipeline for Qt WebContentView");
        return false;
    }
    m_metal_pipeline_state = pipeline_state;

    auto* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    sampler_descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    m_metal_sampler_state = [device newSamplerStateWithDescriptor:sampler_descriptor];
    [sampler_descriptor release];

    if (!m_metal_sampler_state) {
        dbgln("Failed to create Metal sampler for Qt WebContentView");
        return false;
    }

    return true;
}

bool WebContentView::update_imported_iosurface_texture(Gfx::SharedImageBuffer const& shared_image_buffer)
{
    if (m_imported_shared_image_buffer == &shared_image_buffer && m_imported_iosurface_texture)
        return true;

    release_imported_iosurface_texture();

    auto* device = (id<MTLDevice>)m_metal_device;
    if (!device)
        return false;

    auto const& iosurface_handle = shared_image_buffer.iosurface_handle();
    // LibGfx IOSurfaces are BGRA in memory. Match that storage format when
    // importing the IOSurface so Metal can do the channel mapping while sampling.
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                          width:iosurface_handle.width()
                                                                         height:iosurface_handle.height()
                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;

    m_imported_iosurface_texture = [device newTextureWithDescriptor:descriptor
                                                          iosurface:(IOSurfaceRef)iosurface_handle.core_foundation_pointer()
                                                              plane:0];
    if (!m_imported_iosurface_texture)
        return false;

    m_imported_shared_image_buffer = &shared_image_buffer;
    return true;
}

void WebContentView::initialize(QRhiCommandBuffer*)
{
    release_metal_resources();
}

void WebContentView::releaseResources()
{
    release_metal_resources();
}

void WebContentView::render(QRhiCommandBuffer* command_buffer)
{
    auto background_color = page_background_color();
    auto clear_color = QColor(background_color.red(), background_color.green(), background_color.blue());

    command_buffer->beginPass(renderTarget(), clear_color, { 1.0f, 0 }, nullptr, QRhiCommandBuffer::ExternalContent);

    auto paintable = current_paintable();
    if (!paintable.has_value() || paintable->bitmap_size.is_empty() || !rhi() || rhi()->backend() != QRhi::Metal) {
        command_buffer->endPass();
        return;
    }

    auto native_color_texture = colorTexture()->nativeTexture();
    auto* render_target_texture = (id<MTLTexture>)native_color_texture.object;
    if (!render_target_texture || !prepare_metal_renderer(render_target_texture.pixelFormat) || !update_imported_iosurface_texture(*paintable->shared_image_buffer)) {
        command_buffer->endPass();
        return;
    }

    auto target_size = colorTexture()->pixelSize();
    auto content_width = min(paintable->bitmap_size.width(), target_size.width());
    auto content_height = min(paintable->bitmap_size.height(), target_size.height());
    if (content_width <= 0 || content_height <= 0) {
        command_buffer->endPass();
        return;
    }

    struct Uniforms {
        float target_size[2];
        float content_size[2];
        float source_size[2];
    } uniforms {
        { static_cast<float>(target_size.width()), static_cast<float>(target_size.height()) },
        { static_cast<float>(content_width), static_cast<float>(content_height) },
        { static_cast<float>(paintable->shared_image_buffer->iosurface_handle().width()), static_cast<float>(paintable->shared_image_buffer->iosurface_handle().height()) },
    };

    // The pass was opened with ExternalContent so we can encode the Metal draw
    // directly into Qt's command buffer.
    command_buffer->beginExternal();
    auto const* command_buffer_native_handles = static_cast<QRhiMetalCommandBufferNativeHandles const*>(command_buffer->nativeHandles());
    if (command_buffer_native_handles && command_buffer_native_handles->encoder) {
        auto* encoder = command_buffer_native_handles->encoder;
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)m_metal_pipeline_state];
        [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder setFragmentTexture:(id<MTLTexture>)m_imported_iosurface_texture atIndex:0];
        [encoder setFragmentSamplerState:(id<MTLSamplerState>)m_metal_sampler_state atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    command_buffer->endExternal();

    command_buffer->endPass();
}

}
