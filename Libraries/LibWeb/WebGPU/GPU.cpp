/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGPU/GPU.h>
#include <LibWeb/WebGPU/GPUAdapter.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/Instance.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPU);

GPU::GPU(JS::Realm& realm)
    : PlatformObject(realm)
{
    MUST(m_native_gpu.initialize());
}

GPU::~GPU() = default;

JS::ThrowCompletionOr<GC::Ref<GPU>> GPU::create(JS::Realm& realm)
{
    return realm.create<GPU>(realm);
}

void GPU::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPU);
    Base::initialize(realm);
}

void GPU::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpu-requestadapter
GC::Ref<WebIDL::Promise> GPU::request_adapter()
{
    auto& realm = this->realm();
    GC::Ref promise = WebIDL::create_promise(realm);
    NonnullRefPtr native_promise = m_native_gpu.request_adapter();
    native_promise->when_resolved([&realm, promise](WebGPUNative::Adapter& native_gpu_adapter) -> ErrorOr<void> {
        auto gpu_adapter = MUST(GPUAdapter::create(realm, std::move(native_gpu_adapter)));
        auto& gpu_adapter_realm = HTML::relevant_realm(gpu_adapter);
        HTML::TemporaryExecutionContext const context { gpu_adapter_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::resolve_promise(gpu_adapter_realm, promise, gpu_adapter);
        return {};
    });
    native_promise->when_rejected([&realm, promise](Error const& error) {
        HTML::TemporaryExecutionContext const context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::reject_promise(realm, *promise, WebIDL::InvalidStateError::create(realm, MUST(String::formatted("{}", error))));
    });

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, native_promise]() mutable {
        WebGPUNative::Adapter native_gpu_adapter = m_native_gpu.adapter();
        if (auto result = native_gpu_adapter.initialize(); !result.is_error()) {
            native_promise->resolve(std::move(native_gpu_adapter));
        } else {
            native_promise->reject(std::move(result.error()));
        }
    }));
    return promise;
}

}
