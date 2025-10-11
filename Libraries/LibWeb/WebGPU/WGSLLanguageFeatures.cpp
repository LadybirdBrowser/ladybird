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
#include <LibWeb/WebGPU/WGSLLanguageFeatures.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(WGSLLanguageFeatures);

GC::Ref<WGSLLanguageFeatures> WGSLLanguageFeatures::create(JS::Realm& realm)
{
    return realm.create<WGSLLanguageFeatures>(realm);
}

WGSLLanguageFeatures::WGSLLanguageFeatures(JS::Realm& realm)
    : PlatformObject(realm)
    , m_set_entries(JS::Set::create(realm))
{
}

void WGSLLanguageFeatures::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WGSLLanguageFeatures);
    Base::initialize(realm);
}

void WGSLLanguageFeatures::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set_entries);
}

bool WGSLLanguageFeatures::has_state(FlyString const& state) const
{
    return m_set_entries->set_has(JS::PrimitiveString::create(realm().vm(), state));
}

}
