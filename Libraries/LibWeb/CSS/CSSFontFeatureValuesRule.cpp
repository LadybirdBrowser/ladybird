/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

Vector<Utf16FlyString> CSSFontFeatureValuesRule::font_families() const
{
    auto const* rule_values = &values();
    Vector<Utf16FlyString> families;
    auto count = Parser::ValueParserFFI::rust_font_feature_values_family_count(rule_values);
    for (size_t index = 0; index < count; ++index) {
        auto family = Parser::ValueParserFFI::rust_font_feature_values_family_at(rule_values, index);
        families.append(Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(family.utf16), family.length }));
    }
    return families;
}

void CSSFontFeatureValuesRule::clear_caches()
{
    auto const* parent_style_sheet = this->parent_style_sheet();

    if (!parent_style_sheet)
        return;

    auto document = parent_style_sheet->owning_document();

    if (!document)
        return;

    for (auto const& family : font_families()) {
        document->font_computer().clear_computed_font_cache(family);
        document->font_computer().clear_font_feature_values_cache(family);
    }
}

}
