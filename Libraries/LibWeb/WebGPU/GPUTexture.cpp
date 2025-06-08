/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUTexture.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUTexture);

GPUTexture::GPUTexture(JS::Realm& realm, WebGPUNative::Texture texture)
    : PlatformObject(realm)
    , m_native_gpu_texture(std::move(texture))
{
}

JS::ThrowCompletionOr<GC::Ref<GPUTexture>> GPUTexture::create(JS::Realm& realm, WebGPUNative::Texture texture)
{
    return realm.create<GPUTexture>(realm, std::move(texture));
}

ErrorOr<NonnullOwnPtr<WebGPUNative::MappedTextureBuffer>> GPUTexture::map_buffer()
{
    return TRY(m_native_gpu_texture.map_buffer());
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gputexture-createview
GC::Root<GPUTextureView> GPUTexture::create_view(GPUTextureViewDescriptor const&) const
{
    auto native_gpu_texture_view = m_native_gpu_texture.texture_view();
    MUST(native_gpu_texture_view.initialize());
    return MUST(GPUTextureView::create(realm(), std::move(native_gpu_texture_view)));
}

void GPUTexture::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUTexture);
    Base::initialize(realm);
}

void GPUTexture::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
