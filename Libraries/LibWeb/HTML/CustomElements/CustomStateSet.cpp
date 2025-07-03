/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CustomStateSet);

GC::Ref<CustomStateSet> CustomStateSet::create(JS::Realm& realm, GC::Ref<DOM::Element> element)
{
    return realm.create<CustomStateSet>(realm, element);
}

CustomStateSet::CustomStateSet(JS::Realm& realm, GC::Ref<DOM::Element> element)
    : Bindings::PlatformObject(realm)
    , m_set_entries(JS::Set::create(realm))
    , m_element(element)
{
}

void CustomStateSet::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CustomStateSet);
    Base::initialize(realm);
}

void CustomStateSet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set_entries);
    visitor.visit(m_element);
}

bool CustomStateSet::has_state(FlyString const& state) const
{
    return m_set_entries->set_has(JS::PrimitiveString::create(realm().vm(), state));
}

void CustomStateSet::on_set_modified_from_js(Badge<Bindings::CustomStateSetPrototype>)
{
    m_element->invalidate_style(DOM::StyleInvalidationReason::CustomStateSetChange);
}

}
