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

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSNumericArray::is_equal_numeric_values(GC::Ref<CSSNumericArray> other) const
{
    // NB: This is just step 3, moved here to reduce repetition.
    // 3. If value1 and value2 are both CSSMathSums, CSSMathProducts, CSSMathMins, or CSSMathMaxs:
    {
        // 1. If value1’s values and value2s values internal slots have different sizes, return false.
        if (m_values.size() != other->m_values.size())
            return false;

        // 2. If any item in value1’s values internal slot is not an equal numeric value to the item in value2’s values
        //    internal slot at the same index, return false.
        for (auto index = 0u; index < m_values.size(); ++index) {
            if (!m_values[index]->is_equal_numeric_value(other->m_values[index]))
                return false;
        }

        // 3. Return true.
        return true;
    }
}

}
