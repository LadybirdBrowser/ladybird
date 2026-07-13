/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFontFeatureValuesRule.h"
#include <AK/QuickSort.h>
#include <LibWeb/Bindings/CSSFontFeatureValuesRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFeatureValuesRule);

GC::Ref<CSSFontFeatureValuesRule> CSSFontFeatureValuesRule::create(JS::Realm& realm, Vector<Utf16FlyString> font_families)
{
    return realm.create<CSSFontFeatureValuesRule>(realm, move(font_families));
}

bool CSSFontFeatureValuesRule::is_font_feature_value_type_at_keyword(Utf16View keyword)
{
    return keyword.equals_ignoring_ascii_case("stylistic"sv)
        || keyword.equals_ignoring_ascii_case("historical-forms"sv)
        || keyword.equals_ignoring_ascii_case("styleset"sv)
        || keyword.equals_ignoring_ascii_case("character-variant"sv)
        || keyword.equals_ignoring_ascii_case("swash"sv)
        || keyword.equals_ignoring_ascii_case("ornaments"sv)
        || keyword.equals_ignoring_ascii_case("annotation"sv);
}

CSSFontFeatureValuesRule::CSSFontFeatureValuesRule(JS::Realm& realm, Vector<Utf16FlyString> font_families)
    : CSSRule(realm, CSSRule::Type::FontFeatureValues)
    , m_font_families(move(font_families))
    , m_annotation(realm.create<CSSFontFeatureValuesMap>(realm, 1, *this))
    , m_ornaments(realm.create<CSSFontFeatureValuesMap>(realm, 1, *this))
    , m_stylistic(realm.create<CSSFontFeatureValuesMap>(realm, 1, *this))
    , m_swash(realm.create<CSSFontFeatureValuesMap>(realm, 1, *this))
    , m_character_variant(realm.create<CSSFontFeatureValuesMap>(realm, 2, *this))
    , m_styleset(realm.create<CSSFontFeatureValuesMap>(realm, AK::NumericLimits<size_t>::max(), *this))
    , m_historical_forms(realm.create<CSSFontFeatureValuesMap>(realm, 1, *this))
{
}

Utf16String CSSFontFeatureValuesRule::serialized_font_family() const
{
    Utf16StringBuilder builder;

    bool first = true;
    for (auto const& family : m_font_families) {
        if (first)
            first = false;
        else
            builder.append(", "sv);

        if (family.view().contains_any_of(Infra::ASCII_WHITESPACE_CODE_POINTS))
            serialize_a_string(builder, family);
        else
            serialize_an_identifier(builder, family);
    }

    return builder.to_string();
}

Utf16String CSSFontFeatureValuesRule::font_family() const
{
    return serialized_font_family();
}

void CSSFontFeatureValuesRule::set_font_family(Utf16View value)
{
    Vector<Utf16FlyString> family_names;

    value.for_each_split_view(u',', SplitBehavior::Nothing, [&](Utf16View family_name) {
        family_names.append(Utf16FlyString::from_utf16(family_name.trim(Infra::ASCII_WHITESPACE)));
        return IterationDecision::Continue;
    });

    m_font_families = move(family_names);
}

Utf16String CSSFontFeatureValuesRule::serialized() const
{
    Utf16StringBuilder builder;

    auto serialize_font_feature_values_map = [&](CSSFontFeatureValuesMap const& map, StringView const& at_rule_name) {
        if (auto entries = map.to_ordered_hash_map(); !entries.is_empty()) {
            builder.appendff("  @{} {{"sv, at_rule_name);

            for (auto const& [key, value] : entries) {
                builder.append_ascii(' ');
                serialize_an_identifier(builder, key);
                builder.append_ascii(':');

                for (size_t i = 0; i < value.size(); ++i)
                    builder.appendff(" {}", value[i]);

                builder.append_ascii(";"sv);
            }
            builder.append_ascii(" }"sv);
        }
    };

    builder.appendff("@font-feature-values {} {{"sv, serialized_font_family());

    serialize_font_feature_values_map(m_annotation, "annotation"sv);
    serialize_font_feature_values_map(m_ornaments, "ornaments"sv);
    serialize_font_feature_values_map(m_stylistic, "stylistic"sv);
    serialize_font_feature_values_map(m_swash, "swash"sv);
    serialize_font_feature_values_map(m_character_variant, "character-variant"sv);
    serialize_font_feature_values_map(m_styleset, "styleset"sv);
    serialize_font_feature_values_map(m_historical_forms, "historical-forms"sv);
    builder.append_ascii(" }"sv);

    return builder.to_string();
}

HashMap<FontFeatureValueKey, Vector<u32>> CSSFontFeatureValuesRule::to_hash_map() const
{
    HashMap<FontFeatureValueKey, Vector<u32>> map;

    for (auto const& [key, value] : m_annotation->to_ordered_hash_map())
        map.set({ FontFeatureValueType::Annotation, key }, value);

    for (auto const& [key, value] : m_ornaments->to_ordered_hash_map())
        map.set({ FontFeatureValueType::Ornaments, key }, value);

    for (auto const& [key, value] : m_stylistic->to_ordered_hash_map())
        map.set({ FontFeatureValueType::Stylistic, key }, value);

    for (auto const& [key, value] : m_swash->to_ordered_hash_map())
        map.set({ FontFeatureValueType::Swash, key }, value);

    for (auto const& [key, value] : m_character_variant->to_ordered_hash_map())
        map.set({ FontFeatureValueType::CharacterVariant, key }, value);

    for (auto const& [key, value] : m_styleset->to_ordered_hash_map())
        map.set({ FontFeatureValueType::Styleset, key }, value);

    // NB: We don't include historical-forms since it can't be referenced - it seems like it's inclusion in the syntax
    //     for @font-feature-values was a mistake and isn't supported by Chrome or Firefox. See
    //     https://github.com/w3c/csswg-drafts/issues/9926#issuecomment-2017241274

    return map;
}

void CSSFontFeatureValuesRule::clear_caches()
{
    Base::clear_caches();
    auto const* parent_style_sheet = this->parent_style_sheet();

    if (!parent_style_sheet)
        return;

    auto document = parent_style_sheet->owning_document();

    if (!document)
        return;

    for (auto const& family : m_font_families) {
        document->font_computer().clear_computed_font_cache(family);
        document->font_computer().clear_font_feature_values_cache(family);
    }
}

void CSSFontFeatureValuesRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFontFeatureValuesRule);
    Base::initialize(realm);
}

void CSSFontFeatureValuesRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_annotation);
    visitor.visit(m_ornaments);
    visitor.visit(m_stylistic);
    visitor.visit(m_swash);
    visitor.visit(m_character_variant);
    visitor.visit(m_styleset);
    visitor.visit(m_historical_forms);
}

}
