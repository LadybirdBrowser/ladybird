/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/MapIterator.h>
#include <LibWeb/CSS/CSSFontFeatureValuesMap.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustFontFeatureValues.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesRule final : public CSSRule {
    WEB_WRAPPABLE(CSSFontFeatureValuesRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFontFeatureValuesRule);

public:
    static GC::Ref<CSSFontFeatureValuesRule> create(RustRule);

    Utf16String font_family() const;
    void set_font_family(Utf16View);
    GC::Ref<CSSFontFeatureValuesMap> annotation() const { return map(FontFeatureValuesRuleKind::Annotation); }
    GC::Ref<CSSFontFeatureValuesMap> ornaments() const { return map(FontFeatureValuesRuleKind::Ornaments); }
    GC::Ref<CSSFontFeatureValuesMap> stylistic() const { return map(FontFeatureValuesRuleKind::Stylistic); }
    GC::Ref<CSSFontFeatureValuesMap> swash() const { return map(FontFeatureValuesRuleKind::Swash); }
    GC::Ref<CSSFontFeatureValuesMap> character_variant() const { return map(FontFeatureValuesRuleKind::CharacterVariant); }
    GC::Ref<CSSFontFeatureValuesMap> styleset() const { return map(FontFeatureValuesRuleKind::Styleset); }
    GC::Ref<CSSFontFeatureValuesMap> historical_forms() const { return map(FontFeatureValuesRuleKind::HistoricalForms); }

    Vector<Utf16FlyString> font_families() const;
    Parser::ValueParserFFI::FontFeatureValuesRule const& values() const { return m_values; }

    virtual void clear_caches() override;

    virtual Utf16String serialized() const override;

private:
    Utf16String serialized_font_family() const;
    Utf16View family_at(size_t) const;

    CSSFontFeatureValuesRule(RustRule);
    GC::Ref<CSSFontFeatureValuesMap> map(FontFeatureValuesRuleKind) const;

    virtual void visit_edges(Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    Parser::ValueParserFFI::FontFeatureValuesRule const& m_values;
    mutable Array<GC::Ptr<CSSFontFeatureValuesMap>, 7> m_maps;
};

}
