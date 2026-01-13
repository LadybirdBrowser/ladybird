/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFontFeatureValuesMap.h"
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFeatureValuesMap);

GC::Ref<CSSFontFeatureValuesMap> CSSFontFeatureValuesMap::create(JS::Realm& realm, size_t max_value_count)
{
    return realm.create<CSSFontFeatureValuesMap>(realm, max_value_count);
}

CSSFontFeatureValuesMap::CSSFontFeatureValuesMap(JS::Realm& realm, size_t max_value_count)
    : Bindings::PlatformObject(realm)
    , m_map_entries(JS::Map::create(realm))
    , m_max_value_count(max_value_count)
{
}

WebIDL::ExceptionOr<void> CSSFontFeatureValuesMap::set(String const& feature_value_name, Variant<u32, Vector<u32>> const& values)
{
    // https://drafts.csswg.org/css-fonts-4/#cssfontfeaturevaluesmap
    // The CSSFontFeatureValuesMap interface uses the default map class methods but the set method has different
    // behavior. It takes a sequence of unsigned integers and associates it with a given featureValueName. The method
    // behaves the same as the default map class method except that

    // a single unsigned long value is treated as a sequence of a single value.
    Vector<u32> value_vector = values.visit(
        [](u32 single_value) { return Vector<u32> { single_value }; },
        [](Vector<u32> value_vector) { return value_vector; });

    // The method throws an exception if an invalid number of values is passed in.
    if (value_vector.is_empty())
        return WebIDL::InvalidAccessError::create(realm(), "CSSFontFeatureValuesMap.set requires at least one value."_utf16);

    // If the associated feature value block only allows a limited number of values, the set method throws an
    // InvalidAccessError exception when the input sequence to set contains more than the limited number of values. See
    // the description of multi-valued feature value definitions for details on the maximum number of values allowed for
    // a given type of feature value block.
    if (value_vector.size() > m_max_value_count)
        return WebIDL::InvalidAccessError::create(realm(), Utf16String::formatted("CSSFontFeatureValuesMap.set only allows a maximum of {} values for the associated feature", m_max_value_count));

    Vector<JS::Value> wrapped_values;
    wrapped_values.ensure_capacity(value_vector.size());

    for (auto const& value : value_vector)
        wrapped_values.append(JS::Value { value });

    m_map_entries->map_set(JS::PrimitiveString::create(vm(), feature_value_name), JS::Array::create_from(realm(), wrapped_values.span()));
    // TODO: Clear the relevant caches

    return {};
}

void CSSFontFeatureValuesMap::on_map_modified_from_js(Badge<Bindings::CSSFontFeatureValuesMapPrototype>)
{
    // TODO: Clear the relevant caches
}

OrderedHashMap<FlyString, Vector<u32>> CSSFontFeatureValuesMap::to_ordered_hash_map() const
{
    OrderedHashMap<FlyString, Vector<u32>> result;

    for (auto const& entry : *m_map_entries) {
        auto key = MUST(entry.key.to_string(vm()));

        auto const& array = as<JS::Array>(entry.value.as_object());
        auto array_length = MUST(MUST(array.get(vm().names.length)).to_length(vm()));

        Vector<u32> values;
        values.ensure_capacity(array_length);

        for (size_t i = 0; i < array_length; ++i)
            values.append(MUST(array.get_without_side_effects(JS::PropertyKey { i }).to_u32(vm())));

        result.set(key, values);
    }

    return result;
}

void CSSFontFeatureValuesMap::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFontFeatureValuesMap);
    Base::initialize(realm);
}

void CSSFontFeatureValuesMap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_map_entries);
}

}
