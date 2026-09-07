/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibJS/Runtime/MapIterator.h>
#include <LibWeb/Bindings/CSSFontFeatureValuesMap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/RustFontFeatureValues.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesMap final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CSSFontFeatureValuesMap, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CSSFontFeatureValuesMap);

public:
    static GC::Ref<CSSFontFeatureValuesMap> create(FontFeatureValuesRuleKind, GC::Ref<CSSFontFeatureValuesRule> parent_rule);

    size_t map_size() const;
    OrderedHashMap<Utf16String, Vector<u32>> entries() const;
    Optional<Vector<u32>> map_get(Utf16View key) const;
    bool map_has(Utf16View key) const;
    void map_set(Utf16View key, Vector<u32> const& values);
    bool map_remove(Utf16View key);
    void map_clear();

    size_t max_value_count() const;

private:
    CSSFontFeatureValuesMap(FontFeatureValuesRuleKind, GC::Ref<CSSFontFeatureValuesRule> parent_rule);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    FontFeatureValuesRuleKind m_kind;
    GC::Ref<CSSFontFeatureValuesRule> m_parent_rule;
};

}
