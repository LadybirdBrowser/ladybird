/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPURenderPassEncoder.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPURenderPassEncoder);

GPURenderPassEncoder::GPURenderPassEncoder(JS::Realm& realm, GPURenderPassDescriptor const& render_pass_descriptor, WebGPUNative::RenderPassEncoder render_pass_encoder)
    : PlatformObject(realm)
    , m_gpu_render_pass_descriptor(render_pass_descriptor)
    , m_native_gpu_render_pass_encoder(std::move(render_pass_encoder))
{
}

JS::ThrowCompletionOr<GC::Ref<GPURenderPassEncoder>> GPURenderPassEncoder::create(JS::Realm& realm, GPURenderPassDescriptor const& render_pass_descriptor, WebGPUNative::RenderPassEncoder render_pass_encoder)
{
    return realm.create<GPURenderPassEncoder>(realm, render_pass_descriptor, std::move(render_pass_encoder));
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpurenderpassencoder-end
void GPURenderPassEncoder::end()
{
    m_native_gpu_render_pass_encoder.end();
}

void GPURenderPassEncoder::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPURenderPassEncoder);
    Base::initialize(realm);
}

void GPURenderPassEncoder::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
