/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUCommandBuffer.h>
#include <LibWeb/WebGPU/GPUQueue.h>

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
