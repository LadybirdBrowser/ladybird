/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUCommandBuffer.h>
#include <LibWeb/WebGPU/GPUQueue.h>
#include <LibWebGPUNative/CommandBuffer.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUQueue);

GPUQueue::GPUQueue(JS::Realm& realm, WebGPUNative::Queue queue)
    : PlatformObject(realm)
    , m_native_gpu_queue(std::move(queue))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUQueue>> GPUQueue::create(JS::Realm& realm, WebGPUNative::Queue queue)
{
    return realm.create<GPUQueue>(realm, std::move(queue));
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpuqueue-submit
void GPUQueue::submit(Vector<GC::Root<GPUCommandBuffer>> const& command_buffers)
{
    Vector<NonnullRawPtr<WebGPUNative::CommandBuffer>> native_command_buffers;
    for (auto const& command_buffer : command_buffers) {
        native_command_buffers.append(command_buffer->native());
    }
    MUST(m_native_gpu_queue.submit(native_command_buffers));
}

void GPUQueue::on_submitted(Function<void()> callback)
{
    m_native_gpu_queue.on_submitted(std::move(callback));
}

void GPUQueue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUQueue);
    Base::initialize(realm);
}

void GPUQueue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
