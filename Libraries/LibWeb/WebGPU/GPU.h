/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGPU/Native/NativeGPU.h>

namespace Web::WebGPU {

class GPU final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPU, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPU);

public:
    static JS::ThrowCompletionOr<GC::Ref<GPU>> create(JS::Realm&);

    NativeGPU& native_gpu() { return m_native_gpu; }

private:
    explicit GPU(JS::Realm&, NativeGPU);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    NativeGPU m_native_gpu;
};

}
