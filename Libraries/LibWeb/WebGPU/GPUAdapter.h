/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUAdapterPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWebGPUNative/Adapter.h>

namespace Web::WebGPU {

class GPUAdapter final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUAdapter, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUAdapter);

    static JS::ThrowCompletionOr<GC::Ref<GPUAdapter>> create(JS::Realm&, WebGPUNative::Adapter);

private:
    explicit GPUAdapter(JS::Realm&, WebGPUNative::Adapter);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::Adapter m_native_gpu_adapter;
};

}
