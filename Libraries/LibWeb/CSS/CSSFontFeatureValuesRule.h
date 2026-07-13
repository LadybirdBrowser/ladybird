/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/MapIterator.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSFontFeatureValuesMap.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSFontFeatureValuesRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFontFeatureValuesRule);

public:
    static GC::Ref<CSSFontFeatureValuesRule> create(JS::Realm&, Vector<Utf16FlyString> font_families);

    static bool is_font_feature_value_type_at_keyword(Utf16View);

    Utf16String font_family() const;
    void set_font_family(Utf16View);
    GC::Ref<CSSFontFeatureValuesMap> annotation() const { return m_annotation; }
    GC::Ref<CSSFontFeatureValuesMap> ornaments() const { return m_ornaments; }
    GC::Ref<CSSFontFeatureValuesMap> stylistic() const { return m_stylistic; }
    GC::Ref<CSSFontFeatureValuesMap> swash() const { return m_swash; }
    GC::Ref<CSSFontFeatureValuesMap> character_variant() const { return m_character_variant; }
    GC::Ref<CSSFontFeatureValuesMap> styleset() const { return m_styleset; }
    GC::Ref<CSSFontFeatureValuesMap> historical_forms() const { return m_historical_forms; }

    Vector<Utf16FlyString> const& font_families() const { return m_font_families; }
    HashMap<FontFeatureValueKey, Vector<u32>> to_hash_map() const;

    virtual void clear_caches() override;

    virtual Utf16String serialized() const override;

private:
    Utf16String serialized_font_family() const;

    CSSFontFeatureValuesRule(JS::Realm&, Vector<Utf16FlyString> font_families);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<Utf16FlyString> m_font_families;
    GC::Ref<CSSFontFeatureValuesMap> m_annotation;
    GC::Ref<CSSFontFeatureValuesMap> m_ornaments;
    GC::Ref<CSSFontFeatureValuesMap> m_stylistic;
    GC::Ref<CSSFontFeatureValuesMap> m_swash;
    GC::Ref<CSSFontFeatureValuesMap> m_character_variant;
    GC::Ref<CSSFontFeatureValuesMap> m_styleset;
    GC::Ref<CSSFontFeatureValuesMap> m_historical_forms;
};

}
