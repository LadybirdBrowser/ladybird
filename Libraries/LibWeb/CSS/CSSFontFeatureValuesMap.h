/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesMap final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CSSFontFeatureValuesMap, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CSSFontFeatureValuesMap);

public:
    static GC::Ref<CSSFontFeatureValuesMap> create(size_t max_value_count, GC::Ref<CSSFontFeatureValuesRule> parent_rule);

    size_t map_size() const { return m_entries.size(); }
    OrderedHashMap<FlyString, Vector<u32>> const& entries() const { return m_entries; }
    Vector<u32> const* map_get(FlyString const& key) const;
    bool map_has(FlyString const& key) const;
    void map_set(FlyString const& key, Vector<u32> const& values);
    bool map_remove(FlyString const& key);
    void map_clear();

    void set_from_parser(FlyString const& feature_value_name, Vector<u32> values);
    size_t max_value_count() const { return m_max_value_count; }

    OrderedHashMap<FlyString, Vector<u32>> to_ordered_hash_map() const;

private:
    CSSFontFeatureValuesMap(size_t max_value_count, GC::Ref<CSSFontFeatureValuesRule> parent_rule);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    OrderedHashMap<FlyString, Vector<u32>> m_entries;
    size_t m_max_value_count { 0 };
    GC::Ref<CSSFontFeatureValuesRule> m_parent_rule;
};

}
