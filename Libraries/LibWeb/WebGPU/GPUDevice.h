/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUDevicePrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebGPU/GPUCommandEncoder.h>
#include <LibWeb/WebGPU/GPUQueue.h>
#include <LibWebGPUNative/Device.h>

namespace Web::WebGPU {

class GPUDevice final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(GPUDevice, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(GPUDevice);

    static JS::ThrowCompletionOr<GC::Ref<GPUDevice>> create(JS::Realm&, WebGPUNative::Device);

    GC::Ref<GPUQueue> queue() const { return *m_queue; }

    void on_queue_submitted(Function<void()> callback);

    GC::Root<GPUCommandEncoder> create_command_encoder(GPUCommandEncoderDescriptor const&) const;

private:
    explicit GPUDevice(JS::Realm&, WebGPUNative::Device);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::Device m_native_gpu_device;
    GC::Ref<GPUQueue> m_queue;
};

}
