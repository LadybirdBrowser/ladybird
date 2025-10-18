/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/WebGPU/GPUSupportedFeatures.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUSupportedFeatures);

GC::Ref<GPUSupportedFeatures> GPUSupportedFeatures::create(JS::Realm& realm)
{
    return realm.create<GPUSupportedFeatures>(realm);
}

GPUSupportedFeatures::GPUSupportedFeatures(JS::Realm& realm)
    : PlatformObject(realm)
    , m_set_entries(JS::Set::create(realm))
{
}

void GPUSupportedFeatures::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUSupportedFeatures);
    Base::initialize(realm);
}

void GPUSupportedFeatures::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set_entries);
}

bool GPUSupportedFeatures::has_state(FlyString const& state) const
{
    return m_set_entries->set_has(JS::PrimitiveString::create(realm().vm(), state));
}

}
