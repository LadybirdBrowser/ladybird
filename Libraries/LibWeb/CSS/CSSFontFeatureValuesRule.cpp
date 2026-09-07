/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFontFeatureValuesRule.h"
#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFeatureValuesRule);

GC::Ref<CSSFontFeatureValuesRule> CSSFontFeatureValuesRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSFontFeatureValuesRule>(move(rule));
}

CSSFontFeatureValuesRule::CSSFontFeatureValuesRule(RustRule rule)
    : CSSRule(move(rule))
    , m_values(*native_rule().payload().font_feature_values)
{
}

GC::Ref<CSSFontFeatureValuesMap> CSSFontFeatureValuesRule::map(FontFeatureValuesRuleKind kind) const
{
    auto& wrapper = m_maps[to_underlying(kind)];
    if (!wrapper)
        wrapper = CSSFontFeatureValuesMap::create(kind, const_cast<CSSFontFeatureValuesRule&>(*this));
    return *wrapper;
}

Vector<Utf16FlyString> CSSFontFeatureValuesRule::font_families() const
{
    Vector<Utf16FlyString> families;
    auto count = Parser::ValueParserFFI::rust_font_feature_values_family_count(&m_values);
    for (size_t index = 0; index < count; ++index)
        families.append(Utf16FlyString::from_utf16(family_at(index)));
    return families;
}

Utf16View CSSFontFeatureValuesRule::family_at(size_t index) const
{
    auto family = Parser::ValueParserFFI::rust_font_feature_values_family_at(&m_values, index);
    return { reinterpret_cast<char16_t const*>(family.utf16), family.length };
}

Utf16String CSSFontFeatureValuesRule::serialized_font_family() const
{
    Utf16StringBuilder builder;

    bool first = true;
    auto count = Parser::ValueParserFFI::rust_font_feature_values_family_count(&m_values);
    for (size_t index = 0; index < count; ++index) {
        auto family = family_at(index);
        if (first)
            first = false;
        else
            builder.append(", "sv);

        if (family.contains_any_of(Infra::ASCII_WHITESPACE_CODE_POINTS))
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
    Vector<Parser::ValueParserFFI::FfiUtf16View> family_names;

    value.for_each_split_view(u',', SplitBehavior::Nothing, [&](Utf16View family_name) {
        family_names.append(Parser::ffi_utf16_view(family_name.trim(Infra::ASCII_WHITESPACE)));
        return IterationDecision::Continue;
    });

    Parser::ValueParserFFI::rust_font_feature_values_set_families(&m_values, family_names.data(), family_names.size());
}

Utf16String CSSFontFeatureValuesRule::serialized() const
{
    Utf16StringBuilder builder;

    auto serialize_font_feature_values_map = [&](FontFeatureValuesRuleKind kind, StringView const& at_rule_name) {
        auto count = Parser::ValueParserFFI::rust_font_feature_values_count(&m_values, kind);
        if (count != 0) {
            builder.appendff("  @{} {{"sv, at_rule_name);

            for (size_t index = 0; index < count; ++index) {
                auto entry = Parser::ValueParserFFI::rust_font_feature_values_at(&m_values, kind, index);
                builder.append_ascii(' ');
                serialize_an_identifier(builder, { reinterpret_cast<char16_t const*>(entry.name.utf16), entry.name.length });
                builder.append_ascii(':');

                for (size_t i = 0; i < entry.count; ++i)
                    builder.appendff(" {}", entry.values[i]);

                builder.append_ascii(";"sv);
            }
            builder.append_ascii(" }"sv);
        }
    };

    builder.appendff("@font-feature-values {} {{"sv, serialized_font_family());

    serialize_font_feature_values_map(FontFeatureValuesRuleKind::Annotation, "annotation"sv);
    serialize_font_feature_values_map(FontFeatureValuesRuleKind::Ornaments, "ornaments"sv);
    serialize_font_feature_values_map(FontFeatureValuesRuleKind::Stylistic, "stylistic"sv);
    serialize_font_feature_values_map(FontFeatureValuesRuleKind::Swash, "swash"sv);
    serialize_font_feature_values_map(FontFeatureValuesRuleKind::CharacterVariant, "character-variant"sv);
    serialize_font_feature_values_map(FontFeatureValuesRuleKind::Styleset, "styleset"sv);
    serialize_font_feature_values_map(FontFeatureValuesRuleKind::HistoricalForms, "historical-forms"sv);
    builder.append_ascii(" }"sv);

    return builder.to_string();
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

    for (auto const& family : font_families()) {
        document->font_computer().clear_computed_font_cache(family);
        document->font_computer().clear_font_feature_values_cache(family);
    }
}

void CSSFontFeatureValuesRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& map : m_maps)
        visitor.visit(map);
}

size_t CSSFontFeatureValuesRule::external_memory_size() const
{
    return Base::external_memory_size() + Parser::ValueParserFFI::rust_font_feature_values_external_memory_size(&m_values);
}

}
