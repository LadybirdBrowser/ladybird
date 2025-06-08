/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUCommandBuffer.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUCommandBuffer);

GPUCommandBuffer::GPUCommandBuffer(JS::Realm& realm, WebGPUNative::CommandBuffer command_buffer)
    : PlatformObject(realm)
    , m_native_gpu_command_buffer(std::move(command_buffer))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUCommandBuffer>> GPUCommandBuffer::create(JS::Realm& realm, WebGPUNative::CommandBuffer command_buffer)
{
    return realm.create<GPUCommandBuffer>(realm, std::move(command_buffer));
}
void GPUCommandBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUCommandBuffer);
    Base::initialize(realm);
}

void GPUCommandBuffer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
