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

}
