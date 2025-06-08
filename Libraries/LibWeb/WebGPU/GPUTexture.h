/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUTexturePrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWebGPUNative/Texture.h>

namespace Web::WebGPU {

struct GPUTextureViewDescriptor { };

class GPUTexture final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUTexture, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUTexture);

    static JS::ThrowCompletionOr<GC::Ref<GPUTexture>> create(JS::Realm&, WebGPUNative::Texture);

    ErrorOr<NonnullOwnPtr<WebGPUNative::MappedTextureBuffer>> map_buffer();

private:
    explicit GPUTexture(JS::Realm&, WebGPUNative::Texture);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::Texture m_native_gpu_texture;
};

}
