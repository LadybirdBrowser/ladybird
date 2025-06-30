/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUCommandEncoder.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUCommandEncoder);

GPUCommandEncoder::GPUCommandEncoder(JS::Realm& realm, WebGPUNative::CommandEncoder command_encoder)
    : PlatformObject(realm)
    , m_native_gpu_command_encoder(std::move(command_encoder))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUCommandEncoder>> GPUCommandEncoder::create(JS::Realm& realm, WebGPUNative::CommandEncoder command_encoder)
{
    return realm.create<GPUCommandEncoder>(realm, std::move(command_encoder));
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpucommandencoder-beginrenderpass
GC::Root<GPURenderPassEncoder> GPUCommandEncoder::begin_render_pass(GPURenderPassDescriptor const& render_pass_descriptor)
{
    auto const& color_attachments = render_pass_descriptor.color_attachments;
    Vector<WebGPUNative::RenderPassColorAttachment> native_color_attachments;
    for (auto const& color_attachment : color_attachments) {
        Optional<WebGPUNative::Color> native_color;
        auto const& clear_value = color_attachment.clear_value;
        if (clear_value.has_value()) {
            clear_value.value().visit(
                [&](Vector<double> const& rgba) {
                    VERIFY(rgba.size() == 4);
                    native_color.emplace(rgba[0], rgba[1], rgba[2], rgba[3]);
                },
                [&](GPUColorDict const& rgba) {
                    native_color.emplace(rgba.r, rgba.g, rgba.b, rgba.a);
                });
        }
        native_color_attachments.empend(color_attachment.view->native(), native_color);
    }
    WebGPUNative::RenderPassDescriptor native_render_pass_descriptor;
    native_render_pass_descriptor.color_attachments = native_color_attachments;
    auto native_gpu_render_pass_encoder = MUST(m_native_gpu_command_encoder.begin_render_pass(native_render_pass_descriptor));
    return MUST(GPURenderPassEncoder::create(realm(), render_pass_descriptor, std::move(native_gpu_render_pass_encoder)));
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpucommandencoder-finish
GC::Root<GPUCommandBuffer> GPUCommandEncoder::finish(GPUCommandBufferDescriptor const&)
{
    auto native_gpu_command_buffer = MUST(m_native_gpu_command_encoder.finish());
    return MUST(GPUCommandBuffer::create(realm(), std::move(native_gpu_command_buffer)));
}

void GPUCommandEncoder::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUCommandEncoder);
    Base::initialize(realm);
}

void GPUCommandEncoder::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
