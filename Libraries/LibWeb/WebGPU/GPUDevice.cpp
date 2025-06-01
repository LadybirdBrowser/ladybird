/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebGPU/GPUDevice.h>
#include <LibWeb/WebGPU/GPUQueue.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUDevice);

GPUDevice::GPUDevice(JS::Realm& realm, WebGPUNative::Device device)
    : EventTarget(realm)
    , m_native_gpu_device(std::move(device))
    , m_queue(MUST(GPUQueue::create(realm, m_native_gpu_device.queue())))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUDevice>> GPUDevice::create(JS::Realm& realm, WebGPUNative::Device device)
{
    return realm.create<GPUDevice>(realm, std::move(device));
}

void GPUDevice::on_queue_submitted(Function<void()> callback)
{
    m_queue->on_submitted(std::move(callback));
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpudevice-createcommandencoder
GC::Root<GPUCommandEncoder> GPUDevice::create_command_encoder(GPUCommandEncoderDescriptor const&) const
{
    auto native_gpu_command_encoder = m_native_gpu_device.command_encoder();
    MUST(native_gpu_command_encoder.initialize());
    return MUST(GPUCommandEncoder::create(realm(), std::move(native_gpu_command_encoder)));
}

void GPUDevice::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUDevice);
    EventTarget::initialize(realm);
}

void GPUDevice::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_queue);
}

}
