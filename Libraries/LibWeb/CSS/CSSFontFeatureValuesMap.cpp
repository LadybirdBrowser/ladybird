/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFontFeatureValuesMap.h"
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFeatureValuesMap);

GC::Ref<CSSFontFeatureValuesMap> CSSFontFeatureValuesMap::create(size_t max_value_count, GC::Ref<CSSFontFeatureValuesRule> parent_rule)
{
    return GC::Heap::the().allocate<CSSFontFeatureValuesMap>(max_value_count, parent_rule);
}

CSSFontFeatureValuesMap::CSSFontFeatureValuesMap(size_t max_value_count, GC::Ref<CSSFontFeatureValuesRule> parent_rule)
    : m_max_value_count(max_value_count)
    , m_parent_rule(parent_rule)
{
}

static FlyString feature_value_name_from_map_key(JS::Value key)
{
    VERIFY(key.is_string());
    return FlyString { key.as_string().utf8_string() };
}

static Vector<u32> feature_values_from_map_value(JS::Value value)
{
    VERIFY(value.is_object());
    auto const& array = as<JS::Array>(value.as_object());
    auto& vm = HTML::relevant_realm(array).vm();
    auto array_length = MUST(MUST(array.get(vm.names.length)).to_length(vm));

    Vector<u32> values;
    values.ensure_capacity(array_length);

    for (size_t i = 0; i < array_length; ++i)
        values.unchecked_append(MUST(array.get_without_side_effects(JS::PropertyKey { i }).to_u32(vm)));

    return values;
}

static GC::Ref<JS::Array> create_map_value(JS::Realm& realm, Vector<u32> const& values)
{
    Vector<JS::Value> wrapped_values;
    wrapped_values.ensure_capacity(values.size());

    for (auto const& value : values)
        wrapped_values.append(JS::Value { value });

    return JS::Array::create_from(realm, wrapped_values.span());
}

static void set_map_entry(JS::Map& map_entries, FlyString const& feature_value_name, Vector<u32> const& feature_values)
{
    auto& map_realm = HTML::relevant_realm(map_entries);
    auto key = GC::make_root(JS::PrimitiveString::create(map_realm.vm(), feature_value_name));
    auto value = GC::make_root(create_map_value(map_realm, feature_values));
    map_entries.map_set(JS::Value { key.ptr() }, JS::Value { value.ptr() });
}

static void remove_map_entry(JS::Map& map_entries, FlyString const& feature_value_name)
{
    auto& map_realm = HTML::relevant_realm(map_entries);
    auto key = GC::make_root(JS::PrimitiveString::create(map_realm.vm(), feature_value_name));
    map_entries.map_remove(JS::Value { key.ptr() });
}

static GC::Root<JS::Map> create_map_entries(JS::Realm& realm, OrderedHashMap<FlyString, Vector<u32>> const& entries)
{
    auto map_entries = GC::make_root(JS::Map::create(realm));
    for (auto const& entry : entries)
        set_map_entry(*map_entries, entry.key, entry.value);
    return map_entries;
}

GC::Ref<JS::Map> CSSFontFeatureValuesMap::map_entries(JS::Realm& realm, Bindings::WrapperWorld const& wrapper_world) const
{
    if (auto map_entries = m_map_entries.get(wrapper_world))
        return *map_entries;

    auto map_entries = create_map_entries(realm, m_entries);
    m_map_entries.set(wrapper_world, map_entries.ptr());
    return GC::Ref { *map_entries };
}

Optional<JS::Value> CSSFontFeatureValuesMap::map_get(JS::Realm& realm, JS::Value key) const
{
    auto it = m_entries.find(feature_value_name_from_map_key(key));
    if (it == m_entries.end())
        return {};

    return JS::Value { create_map_value(realm, it->value).ptr() };
}

bool CSSFontFeatureValuesMap::map_has(JS::Value key) const
{
    return m_entries.contains(feature_value_name_from_map_key(key));
}

void CSSFontFeatureValuesMap::map_set(JS::Value key, JS::Value value)
{
    auto feature_value_name = feature_value_name_from_map_key(key);
    auto feature_values = feature_values_from_map_value(value);

    m_entries.set(feature_value_name, feature_values);

    m_map_entries.for_each([&](auto& map_entries) {
        set_map_entry(map_entries, feature_value_name, feature_values);
    });
}

bool CSSFontFeatureValuesMap::map_remove(JS::Value key)
{
    auto feature_value_name = feature_value_name_from_map_key(key);
    auto removed = m_entries.remove(feature_value_name);

    if (removed) {
        m_map_entries.for_each([&](auto& map_entries) {
            remove_map_entry(map_entries, feature_value_name);
        });
    }

    return removed;
}

void CSSFontFeatureValuesMap::map_clear()
{
    m_entries.clear();

    m_map_entries.for_each([&](auto& map_entries) {
        map_entries.map_clear();
    });
}

WebIDL::ExceptionOr<void> CSSFontFeatureValuesMap::set(JS::Realm& realm, String const& feature_value_name, Variant<u32, Vector<u32>> const& values)
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
        return WebIDL::InvalidAccessError::create(realm, "CSSFontFeatureValuesMap.set requires at least one value."_utf16);

    // If the associated feature value block only allows a limited number of values, the set method throws an
    // InvalidAccessError exception when the input sequence to set contains more than the limited number of values. See
    // the description of multi-valued feature value definitions for details on the maximum number of values allowed for
    // a given type of feature value block.
    if (value_vector.size() > m_max_value_count)
        return WebIDL::InvalidAccessError::create(realm, Utf16String::formatted("CSSFontFeatureValuesMap.set only allows a maximum of {} values for the associated feature", m_max_value_count));

    m_entries.set(FlyString { feature_value_name }, value_vector);
    m_map_entries.for_each([&](auto& map_entries) {
        set_map_entry(map_entries, FlyString { feature_value_name }, value_vector);
    });

    m_parent_rule->clear_caches();

    return {};
}

void CSSFontFeatureValuesMap::set_from_parser(FlyString const& feature_value_name, Vector<u32> values)
{
    m_entries.set(feature_value_name, values);
    m_map_entries.for_each([&](auto& map_entries) {
        set_map_entry(map_entries, feature_value_name, values);
    });

    m_parent_rule->clear_caches();
}

void CSSFontFeatureValuesMap::on_map_modified_from_js(Badge<Bindings::CSSFontFeatureValuesMapPrototype>)
{
    m_parent_rule->clear_caches();
}

OrderedHashMap<FlyString, Vector<u32>> CSSFontFeatureValuesMap::to_ordered_hash_map() const
{
    return m_entries;
}

void CSSFontFeatureValuesMap::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_rule);
}

}
