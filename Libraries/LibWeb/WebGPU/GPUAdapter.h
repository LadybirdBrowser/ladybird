/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUAdapterPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace wgpu {

class Adapter;
class Instance;
struct RequestAdapterOptions;

}

namespace Web::WebGPU {

struct GPURequestAdapterOptions {
    String feature_level = "core"_string;
    Bindings::GPUPowerPreference power_preference;
    bool force_fallback_adapter = false;
    wgpu::RequestAdapterOptions to_wgpu() const;
};

class GPUAdapter final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUAdapter, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUAdapter);

    // https://www.w3.org/TR/webgpu/#dom-adapter-state-slot
    enum class State {
        Valid,
        Consumed,
        Expired,
    };

    static JS::ThrowCompletionOr<GC::Ref<GPUAdapter>> create(JS::Realm&, wgpu::Instance, wgpu::Adapter);

    ~GPUAdapter() override;

    State state() const;
    void set_state(State value);

    GC::Ref<GPUSupportedFeatures> features() const;

    GC::Ref<GPUSupportedLimits> limits() const;

    GC::Ref<GPUAdapterInfo> info() const;

    GC::Ref<WebIDL::Promise> request_device(GPUDeviceDescriptor const&);

private:
    struct Impl;
    explicit GPUAdapter(JS::Realm&, Impl);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    NonnullOwnPtr<Impl> m_impl;
};

}
