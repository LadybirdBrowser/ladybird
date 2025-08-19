/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNumericArray.h"
#include <LibWeb/Bindings/CSSNumericArrayPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNumericArray);

GC::Ref<CSSNumericArray> CSSNumericArray::create(JS::Realm& realm, Vector<GC::Ref<CSSNumericValue>> values)
{
    return realm.create<CSSNumericArray>(realm, move(values));
}

CSSNumericArray::CSSNumericArray(JS::Realm& realm, Vector<GC::Ref<CSSNumericValue>> values)
    : Bindings::PlatformObject(realm)
    , m_values(move(values))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
    };
}

CSSNumericArray::~CSSNumericArray() = default;

void CSSNumericArray::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSNumericArray);
    Base::initialize(realm);
}

void CSSNumericArray::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericarray-length
WebIDL::UnsignedLong CSSNumericArray::length() const
{
    // The length attribute of CSSNumericArray indicates how many CSSNumericValues are contained within the CSSNumericArray.
    return m_values.size();
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericarray-indexed-property-getter
Optional<JS::Value> CSSNumericArray::item_value(size_t index) const
{
    // The indexed property getter of CSSNumericArray retrieves the CSSNumericValue at the provided index.
    if (auto item = m_values.get(index); item.has_value())
        return item.release_value();
    return {};
}

}
