/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWebGPUNative/Instance.h>

namespace Web::WebGPU {

class GPU final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPU, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPU);

public:
    static JS::ThrowCompletionOr<GC::Ref<GPU>> create(JS::Realm&);

    ~GPU() override;

    GC::Ref<WebIDL::Promise> request_adapter();

private:
    explicit GPU(JS::Realm&);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    WebGPUNative::Instance m_native_gpu;
};

}
