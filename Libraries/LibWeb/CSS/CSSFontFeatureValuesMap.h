/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/CSSFontFeatureValuesMap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesMap final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CSSFontFeatureValuesMap, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CSSFontFeatureValuesMap);

public:
    static GC::Ref<CSSFontFeatureValuesMap> create(size_t max_value_count, GC::Ref<CSSFontFeatureValuesRule> parent_rule);

    size_t map_size() const { return m_entries.size(); }
    GC::Ref<JS::Map> map_entries(JS::Realm&, Bindings::WrapperWorld const&) const;
    Optional<JS::Value> map_get(JS::Realm&, JS::Value key) const;
    bool map_has(JS::Value key) const;
    void map_set(JS::Value key, JS::Value value);
    bool map_remove(JS::Value key);
    void map_clear();

    WebIDL::ExceptionOr<void> set(JS::Realm&, String const& feature_value_name, Variant<u32, Vector<u32>> const& values);
    void set_from_parser(FlyString const& feature_value_name, Vector<u32> values);

    void on_map_modified_from_js(Badge<Bindings::CSSFontFeatureValuesMapPrototype>);

    OrderedHashMap<FlyString, Vector<u32>> to_ordered_hash_map() const;

private:
    CSSFontFeatureValuesMap(size_t max_value_count, GC::Ref<CSSFontFeatureValuesRule> parent_rule);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    OrderedHashMap<FlyString, Vector<u32>> m_entries;
    mutable Bindings::WrapperWorldWeakValueCache<JS::Map> m_map_entries;
    size_t m_max_value_count { 0 };
    GC::Ref<CSSFontFeatureValuesRule> m_parent_rule;
};

}
