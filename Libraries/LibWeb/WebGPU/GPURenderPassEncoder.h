/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPURenderPassEncoderPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGPU/GPUTextureView.h>
#include <LibWebGPUNative/RenderPassEncoder.h>

namespace Web::WebGPU {

struct GPUColorDict {
    double r = { 0.0 };
    double g = { 0.0 };
    double b = { 0.0 };
    double a = { 0.0 };
};

using GPUColor = Variant<Vector<double>, GPUColorDict>;

struct GPURenderPassColorAttachment {
    GC::Root<GPUTextureView> view;
    Optional<GPUColor> clear_value;
    // FIXME: Expose remaining properties
};

struct GPURenderPassDescriptor {
    Vector<GPURenderPassColorAttachment> color_attachments;
};

class GPURenderPassEncoder final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPURenderPassEncoder, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPURenderPassEncoder);

    static JS::ThrowCompletionOr<GC::Ref<GPURenderPassEncoder>> create(JS::Realm&, GPURenderPassDescriptor const&, WebGPUNative::RenderPassEncoder);

    void end();

private:
    explicit GPURenderPassEncoder(JS::Realm&, GPURenderPassDescriptor const&, WebGPUNative::RenderPassEncoder);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    GPURenderPassDescriptor m_gpu_render_pass_descriptor;
    WebGPUNative::RenderPassEncoder m_native_gpu_render_pass_encoder;
};

}
