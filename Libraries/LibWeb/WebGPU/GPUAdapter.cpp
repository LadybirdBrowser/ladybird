/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGPU/GPUAdapter.h>
#include <LibWeb/WebGPU/GPUDevice.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUAdapter);

GPUAdapter::GPUAdapter(JS::Realm& realm, WebGPUNative::Adapter adapter)
    : PlatformObject(realm)
    , m_native_gpu_adapter(std::move(adapter))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUAdapter>> GPUAdapter::create(JS::Realm& realm, WebGPUNative::Adapter adapter)
{
    return realm.create<GPUAdapter>(realm, std::move(adapter));
}

void GPUAdapter::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUAdapter);
    Base::initialize(realm);
}

void GPUAdapter::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpuadapter-requestdevice
GC::Ref<WebIDL::Promise> GPUAdapter::request_device()
{
    auto& realm = this->realm();
    GC::Ref promise = WebIDL::create_promise(realm);
    NonnullRefPtr native_promise = m_native_gpu_adapter.request_device();
    native_promise->when_resolved([&realm, promise](WebGPUNative::Device& native_gpu_device) -> ErrorOr<void> {
        auto gpu_device = MUST(GPUDevice::create(realm, std::move(native_gpu_device)));
        auto& gpu_device_realm = HTML::relevant_realm(gpu_device);
        HTML::TemporaryExecutionContext const context { gpu_device_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::resolve_promise(gpu_device_realm, promise, gpu_device);
        return {};
    });
    native_promise->when_rejected([&realm, promise](Error const& error) {
        HTML::TemporaryExecutionContext const context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::reject_promise(realm, *promise, WebIDL::InvalidStateError::create(realm, MUST(String::formatted("{}", error))));
    });

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, native_promise]() mutable {
        WebGPUNative::Device native_gpu_device = m_native_gpu_adapter.device();
        if (auto result = native_gpu_device.initialize(); !result.is_error()) {
            native_promise->resolve(std::move(native_gpu_device));
        } else {
            native_promise->reject(std::move(result.error()));
        }
    }));
    return promise;
}

}
