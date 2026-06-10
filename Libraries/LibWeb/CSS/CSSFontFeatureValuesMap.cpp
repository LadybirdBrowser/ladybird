/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFontFeatureValuesMap.h"
#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/WrapperWorld.h>
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

Vector<u32> const* CSSFontFeatureValuesMap::map_get(FlyString const& key) const
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return nullptr;

    return &it->value;
}

bool CSSFontFeatureValuesMap::map_has(FlyString const& key) const
{
    return m_entries.contains(key);
}

void CSSFontFeatureValuesMap::map_set(FlyString const& key, Vector<u32> const& values)
{
    m_entries.set(key, values);
    m_parent_rule->clear_caches();
}

bool CSSFontFeatureValuesMap::map_remove(FlyString const& key)
{
    auto removed = m_entries.remove(key);
    m_parent_rule->clear_caches();
    return removed;
}

void CSSFontFeatureValuesMap::map_clear()
{
    m_entries.clear();
    m_parent_rule->clear_caches();
}

void CSSFontFeatureValuesMap::set_from_parser(FlyString const& feature_value_name, Vector<u32> values)
{
    map_set(feature_value_name, values);
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

namespace Web::Bindings {

static WrapperWorldWeakValueCacheMap<CSS::CSSFontFeatureValuesMap, JS::Map>& css_font_feature_values_map_caches()
{
    static NeverDestroyed<WrapperWorldWeakValueCacheMap<CSS::CSSFontFeatureValuesMap, JS::Map>> caches;
    return *caches;
}

static WrapperWorldWeakValueCache<JS::Map>& entries_cache_for(CSS::CSSFontFeatureValuesMap& map)
{
    return css_font_feature_values_map_caches().cache_for(map);
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

static GC::Root<JS::Map> create_map_entries(JS::Realm& realm, CSS::CSSFontFeatureValuesMap const& map)
{
    auto map_entries = GC::make_root(JS::Map::create(realm));
    for (auto const& entry : map.entries())
        set_map_entry(*map_entries, entry.key, entry.value);
    return map_entries;
}

GC::Ref<JS::Map> map_entries(JS::Realm& realm, CSS::CSSFontFeatureValuesMap& map)
{
    auto& wrapper_world = host_defined_wrapper_world(realm);
    auto& cache = entries_cache_for(map);

    if (auto map_entries = cache.get(wrapper_world))
        return *map_entries;

    auto map_entries = create_map_entries(realm, map);
    cache.set(wrapper_world, map_entries.ptr());
    return GC::Ref { *map_entries };
}

Optional<JS::Value> map_get(JS::Realm& realm, CSS::CSSFontFeatureValuesMap& map, FlyString const& key)
{
    auto values = map.map_get(key);
    if (!values)
        return {};

    return JS::Value { create_map_value(realm, *values).ptr() };
}

bool map_has(CSS::CSSFontFeatureValuesMap& map, FlyString const& key)
{
    return map.map_has(key);
}

static void update_cached_entries(CSS::CSSFontFeatureValuesMap& map, FlyString const& key, Vector<u32> const& values)
{
    entries_cache_for(map).for_each([&](auto& map_entries) {
        set_map_entry(map_entries, key, values);
    });
}

bool map_remove(CSS::CSSFontFeatureValuesMap& map, FlyString const& key)
{
    auto removed = map.map_remove(key);
    if (removed) {
        entries_cache_for(map).for_each([&](auto& map_entries) {
            remove_map_entry(map_entries, key);
        });
    }
    return removed;
}

void map_clear(CSS::CSSFontFeatureValuesMap& map)
{
    map.map_clear();
    entries_cache_for(map).for_each([&](auto& map_entries) {
        map_entries.map_clear();
    });
}

WebIDL::ExceptionOr<void> set(JS::Realm&, CSS::CSSFontFeatureValuesMap& map, String const& feature_value_name, Variant<u32, Vector<u32>> const& values)
{
    // https://drafts.csswg.org/css-fonts-4/#cssfontfeaturevaluesmap
    // The CSSFontFeatureValuesMap interface uses the default map class methods but the set method has different
    // behavior. It takes a sequence of unsigned integers and associates it with a given featureValueName. The method
    // behaves the same as the default map class method except that

    // A single unsigned long value is treated as a sequence of a single value.
    Vector<u32> value_vector = values.visit(
        [](u32 single_value) { return Vector<u32> { single_value }; },
        [](Vector<u32> value_vector) { return value_vector; });

    // The method throws an exception if an invalid number of values is passed in.
    if (value_vector.is_empty())
        return WebIDL::InvalidAccessError::create("CSSFontFeatureValuesMap.set requires at least one value."_utf16);

    // If the associated feature value block only allows a limited number of values, the set method throws an
    // InvalidAccessError exception when the input sequence to set contains more than the limited number of values. See
    // the description of multi-valued feature value definitions for details on the maximum number of values allowed for
    // a given type of feature value block.
    if (value_vector.size() > map.max_value_count())
        return WebIDL::InvalidAccessError::create(Utf16String::formatted("CSSFontFeatureValuesMap.set only allows a maximum of {} values for the associated feature", map.max_value_count()));

    FlyString key { feature_value_name };
    map.map_set(key, value_vector);
    update_cached_entries(map, key, value_vector);
    return {};
}

}
