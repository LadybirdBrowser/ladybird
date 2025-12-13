/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPU.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPU);

GPU::GPU(JS::Realm& realm)
    : PlatformObject(realm)
    , m_native_gpu(NativeGPU::create())
{
}

void GPU::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPU);
    Base::initialize(realm);
}

void GPU::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
