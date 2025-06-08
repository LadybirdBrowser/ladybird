/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUCommandEncoder.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUCommandEncoder);

GPUCommandEncoder::GPUCommandEncoder(JS::Realm& realm, WebGPUNative::CommandEncoder command_encoder)
    : PlatformObject(realm)
    , m_native_gpu_command_encoder(std::move(command_encoder))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUCommandEncoder>> GPUCommandEncoder::create(JS::Realm& realm, WebGPUNative::CommandEncoder command_encoder)
{
    return realm.create<GPUCommandEncoder>(realm, std::move(command_encoder));
}

void GPUCommandEncoder::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUCommandEncoder);
    Base::initialize(realm);
}

void GPUCommandEncoder::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
