/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/MapIterator.h>
#include <LibWeb/Bindings/CSSFontFeatureValuesMapPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesMap final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSFontFeatureValuesMap, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSFontFeatureValuesMap);

public:
    static GC::Ref<CSSFontFeatureValuesMap> create(JS::Realm&, size_t max_value_count);

    virtual GC::Ref<JS::Map> map_entries() { return m_map_entries; }

    WebIDL::ExceptionOr<void> set(String const& feature_value_name, Variant<u32, Vector<u32>> const& values);

    void on_map_modified_from_js(Badge<Bindings::CSSFontFeatureValuesMapPrototype>);

    OrderedHashMap<FlyString, Vector<u32>> to_ordered_hash_map() const;

private:
    CSSFontFeatureValuesMap(JS::Realm&, size_t max_value_count);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<JS::Map> m_map_entries;
    size_t m_max_value_count { 0 };
};

}
