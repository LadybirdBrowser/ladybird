/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUAdapterPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGPU/DawnWebGPUForward.h>

namespace Web::WebGPU {

struct GPURequestAdapterOptions {
    String feature_level = "core"_string;
    Bindings::GPUPowerPreference power_preference;
    bool force_fallback_adapter = false;
    bool xr_compatible = false;
};

class GPUAdapter final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUAdapter, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUAdapter);

    static JS::ThrowCompletionOr<GC::Ref<GPUAdapter>> create(JS::Realm&, GPU&, wgpu::Adapter);

    ~GPUAdapter() override;

    GC::Ref<GPUSupportedFeatures> features() const;

    GC::Ref<GPUSupportedLimits> limits() const;

    GC::Ref<GPUAdapterInfo> info() const;

private:
    struct Impl;
    explicit GPUAdapter(JS::Realm&, Impl);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    NonnullOwnPtr<Impl> m_impl;
};

}
