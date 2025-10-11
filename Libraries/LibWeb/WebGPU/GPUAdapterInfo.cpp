/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUAdapterInfo.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUAdapterInfo);

GPUAdapterInfo::GPUAdapterInfo(JS::Realm& realm, String vendor, String architecture, String device, String description, size_t const subgroup_min_size, size_t const subgroup_max_size)
    : PlatformObject(realm)
    , m_vendor(move(vendor))
    , m_architecture(move(architecture))
    , m_device(move(device))
    , m_description(move(description))
    , m_subgroup_min_size(subgroup_min_size)
    , m_subgroup_max_size(subgroup_max_size)
{
}

JS::ThrowCompletionOr<GC::Ref<GPUAdapterInfo>> GPUAdapterInfo::create(JS::Realm& realm, String vendor, String architecture, String device, String description, size_t const subgroup_min_size, size_t const subgroup_max_size)
{
    // FIXME: Set is_fallback_adapter
    return realm.create<GPUAdapterInfo>(realm, move(vendor), move(architecture), move(device), move(description), subgroup_min_size, subgroup_max_size);
}

void GPUAdapterInfo::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUAdapterInfo);
    Base::initialize(realm);
}

void GPUAdapterInfo::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
