/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGPU/DawnWebGPUForward.h>
#include <LibWeb/WebGPU/GPUAdapter.h>

namespace Web::WebGPU {

class GPU final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPU, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPU);

public:
    static JS::ThrowCompletionOr<GC::Ref<GPU>> create(JS::Realm&);

    ~GPU() override;

    wgpu::Instance wgpu() const;

    GC::Ref<WebIDL::Promise> request_adapter(Optional<GPURequestAdapterOptions> options = {});

    Bindings::GPUTextureFormat get_preferred_canvas_format() const;

    GC::Ref<WGSLLanguageFeatures> wgsl_language_features() const;

private:
    struct Impl;
    explicit GPU(JS::Realm&, Impl);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    NonnullOwnPtr<Impl> m_impl;
};

}
