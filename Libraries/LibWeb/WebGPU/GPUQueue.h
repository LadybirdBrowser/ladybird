/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUQueuePrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWebGPUNative/Queue.h>

namespace Web::WebGPU {

class GPUQueue final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUQueue, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUQueue);

    static JS::ThrowCompletionOr<GC::Ref<GPUQueue>> create(JS::Realm&, WebGPUNative::Queue);

private:
    explicit GPUQueue(JS::Realm&, WebGPUNative::Queue);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::Queue m_native_gpu_queue;
};

}
