/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUCommandEncoderPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWebGPUNative/CommandEncoder.h>

namespace Web::WebGPU {

struct GPUCommandEncoderDescriptor { };

class GPUCommandEncoder final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUCommandEncoder, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUCommandEncoder);

    static JS::ThrowCompletionOr<GC::Ref<GPUCommandEncoder>> create(JS::Realm&, WebGPUNative::CommandEncoder);

private:
    explicit GPUCommandEncoder(JS::Realm&, WebGPUNative::CommandEncoder);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::CommandEncoder m_native_gpu_command_encoder;
};

}
