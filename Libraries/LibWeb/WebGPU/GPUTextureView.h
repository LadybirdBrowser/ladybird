/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRawPtr.h>
#include <LibWeb/Bindings/GPUTextureViewPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWebGPUNative/TextureView.h>

namespace Web::WebGPU {

class GPUTextureView final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUTextureView, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUTextureView);

    static JS::ThrowCompletionOr<GC::Ref<GPUTextureView>> create(JS::Realm&, WebGPUNative::TextureView);

    WebGPUNative::TextureView& native()
    {
        return m_native_gpu_texture_view;
    }

private:
    explicit GPUTextureView(JS::Realm&, WebGPUNative::TextureView);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::TextureView m_native_gpu_texture_view;
};

}
