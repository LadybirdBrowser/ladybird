/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRawPtr.h>
#include <LibWeb/Bindings/GPUCommandBufferPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWebGPUNative/CommandBuffer.h>

namespace Web::WebGPU {

struct GPUCommandBufferDescriptor { };

class GPUCommandBuffer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUCommandBuffer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUCommandBuffer);

    static JS::ThrowCompletionOr<GC::Ref<GPUCommandBuffer>> create(JS::Realm&, WebGPUNative::CommandBuffer);

    WebGPUNative::CommandBuffer& native() { return m_native_gpu_command_buffer; }

private:
    explicit GPUCommandBuffer(JS::Realm&, WebGPUNative::CommandBuffer);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::CommandBuffer m_native_gpu_command_buffer;
};

}
