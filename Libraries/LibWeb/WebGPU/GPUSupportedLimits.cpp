/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUSupportedLimits.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUSupportedLimits);

GPUSupportedLimits::GPUSupportedLimits(JS::Realm& realm)
    : PlatformObject(realm)
{
}

JS::ThrowCompletionOr<GC::Ref<GPUSupportedLimits>> GPUSupportedLimits::create(JS::Realm& realm)
{
    return realm.create<GPUSupportedLimits>(realm);
}

void GPUSupportedLimits::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUSupportedLimits);
    Base::initialize(realm);
}

void GPUSupportedLimits::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
