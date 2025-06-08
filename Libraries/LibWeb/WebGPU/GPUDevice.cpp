/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebGPU/GPUDevice.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUDevice);

GPUDevice::GPUDevice(JS::Realm& realm, WebGPUNative::Device device)
    : EventTarget(realm)
    , m_native_gpu_device(std::move(device))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUDevice>> GPUDevice::create(JS::Realm& realm, WebGPUNative::Device device)
{
    return realm.create<GPUDevice>(realm, std::move(device));
}

void GPUDevice::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUDevice);
    EventTarget::initialize(realm);
}

void GPUDevice::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
