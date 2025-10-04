/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUDevicePrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGPU/DawnWebGPUForward.h>
#include <LibWeb/WebGPU/GPUObjectBase.h>
#include <LibWeb/WebGPU/GPUQueue.h>

namespace Web::WebGPU {

struct GPUDeviceDescriptor : GPUObjectDescriptorBase {
    // FIXME: Vector<Bindings::GPUFeatureName> required_features;
    // FIXME: OrderedHashMap<String, Variant<GPUSize64, Empty>> required_limits;
    GPUQueueDescriptor default_queue {};
};

class GPUDevice final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(GPUDevice, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(GPUDevice);

    static JS::ThrowCompletionOr<GC::Ref<GPUDevice>> create(JS::Realm&, GPU&, wgpu::Device, String const&);

    ~GPUDevice() override;

    String const& label() const;
    void set_label(String const& label);

    GC::Ref<GPUQueue> queue() const;

private:
    struct Impl;
    explicit GPUDevice(JS::Realm&, Impl);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    NonnullOwnPtr<Impl> m_impl;
};

}
