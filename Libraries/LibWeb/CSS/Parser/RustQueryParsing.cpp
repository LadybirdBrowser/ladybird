/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/EnvironmentVariable.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/QueryValueType.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

using namespace ValueParserFFI;

static FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

Optional<Vector<RustQueryParser::SizesAttributeEntry>> RustQueryParser::split_sizes_attribute(Utf16View source)
{
    Vector<SizesAttributeEntry> entries;
    auto visit = [](void* context, u16 const* condition, size_t condition_length, u16 const* size, size_t size_length) {
        auto& entries = *static_cast<Vector<SizesAttributeEntry>*>(context);
        entries.append({
            .condition = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(condition), condition_length }),
            .size = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(size), size_length }),
        });
    };
    if (!rust_visit_sizes_attribute_entries(ffi_utf16_view(source), &entries, visit))
        return {};
    return entries;
}

static u16 resolve_query_feature(u8 kind, u16 const* code_units, size_t length)
{
    auto name = Utf16View { reinterpret_cast<char16_t const*>(code_units), length };
    if (kind == 0) {
        auto id = media_feature_id_from_string(name);
        if (!id.has_value())
            return NumericLimits<u16>::max();
        return to_underlying(*id) | (media_feature_type_is_range(*id) ? 0x100 : 0);
    }
    if (kind == 1) {
        auto id = size_feature_id_from_string(name);
        if (!id.has_value())
            return NumericLimits<u16>::max();
        return to_underlying(*id) | (size_feature_type_is_range(*id) ? 0x100 : 0);
    }
    VERIFY(kind == 2);
    return PropertyNameAndID::from_name(Utf16FlyString::from_utf16(name)).has_value() ? 0 : NumericLimits<u16>::max();
}

static bool at_rule_is_supported(Utf16View name)
{
    if (name.equals_ignoring_ascii_case("charset"sv))
        return false;
    if (name.equals_ignoring_ascii_case("container"sv)
        || name.equals_ignoring_ascii_case("counter-style"sv)
        || name.equals_ignoring_ascii_case("font-face"sv)
        || name.equals_ignoring_ascii_case("font-feature-values"sv)
        || name.equals_ignoring_ascii_case("function"sv)
        || name.equals_ignoring_ascii_case("import"sv)
        || name.equals_ignoring_ascii_case("keyframes"sv)
        || name.equals_ignoring_ascii_case("-webkit-keyframes"sv)
        || name.equals_ignoring_ascii_case("layer"sv)
        || name.equals_ignoring_ascii_case("media"sv)
        || name.equals_ignoring_ascii_case("namespace"sv)
        || name.equals_ignoring_ascii_case("page"sv)
        || name.equals_ignoring_ascii_case("property"sv)
        || name.equals_ignoring_ascii_case("scope"sv)
        || name.equals_ignoring_ascii_case("supports"sv))
        return true;
    auto fly_string = Utf16FlyString::from_utf16(name);
    return CSSFontFeatureValuesRule::is_font_feature_value_type_at_keyword(fly_string)
        || is_margin_rule_name(fly_string);
}

Vector<NonnullRefPtr<MediaQuery>> RustQueryParser::parse_media_query_list(Parser&, Utf16View source)
{
    Vector<NonnullRefPtr<MediaQuery>> queries;
    auto visit = [](void* context, FfiQueryHandle const* handle) {
        auto& queries = *static_cast<Vector<NonnullRefPtr<MediaQuery>>*>(context);
        queries.append(MediaQuery::create(RustQueryHandle::retained(handle)));
    };
    if (!rust_visit_media_query_list(ffi_utf16_view(source), resolve_query_feature, &queries, visit))
        return { MediaQuery::create_not_all() };
    return queries;
}

Vector<NonnullRefPtr<MediaQuery>> Parser::parse_as_media_query_list()
{
    return RustQueryParser::parse_media_query_list(*this, m_source);
}

RefPtr<MediaQuery> Parser::parse_as_media_query()
{
    auto media_query_list = parse_as_media_query_list();
    if (media_query_list.is_empty())
        return MediaQuery::create_not_all();
    if (media_query_list.size() == 1)
        return media_query_list.first();
    return nullptr;
}

Optional<RustQueryHandle> RustQueryParser::parse_media_condition(Parser&, Utf16View source)
{
    auto* handle = rust_parse_media_condition(ffi_utf16_view(source), resolve_query_feature);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_media_feature(Parser&, Utf16View source)
{
    auto* handle = rust_parse_media_feature(ffi_utf16_view(source), resolve_query_feature);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_supports_condition(Parser& parser, Utf16View source)
{
    parser.m_rule_context.append(RuleContext::SupportsCondition);
    ScopeGuard pop_context = [&] { parser.m_rule_context.take_last(); };
    auto* handle = rust_parse_supports_condition(ffi_utf16_view(source), &parser, evaluate_supports_feature);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_supports_declaration(Parser& parser, Utf16View source)
{
    parser.m_rule_context.append(RuleContext::SupportsCondition);
    ScopeGuard pop_context = [&] { parser.m_rule_context.take_last(); };
    auto* handle = rust_parse_supports_declaration(ffi_utf16_view(source), &parser, evaluate_supports_feature);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_style_query(Parser&, Utf16View source)
{
    auto* handle = rust_parse_style_query(ffi_utf16_view(source), resolve_query_feature);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<Vector<RustQueryParser::ContainerCondition>> RustQueryParser::parse_container_condition_list(Parser&, Utf16View source)
{
    Vector<ContainerCondition> conditions;
    auto visit = [](void* context, u16 const* name, size_t name_length, bool has_name, FfiQueryHandle const* handle) {
        auto& conditions = *static_cast<Vector<ContainerCondition>*>(context);
        Optional<Utf16FlyString> condition_name;
        if (has_name)
            condition_name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name), name_length });
        RefPtr<ContainerQuery> query;
        if (handle)
            query = ContainerQuery::create(RustQueryHandle::retained(handle));
        conditions.append({ .name = move(condition_name), .query = move(query) });
    };
    if (!rust_visit_container_condition_list(ffi_utf16_view(source), resolve_query_feature, &conditions, visit))
        return {};
    return conditions;
}

RefPtr<StyleValue const> RustQueryParser::parse_source_size_value(Parser& parser, Utf16View source)
{
    auto keyword = ValueParserFFI::rust_parse_css_keyword_from_source(ffi_utf16_view(source));
    if (keyword == to_underlying(Keyword::Auto))
        return KeywordStyleValue::create(Keyword::Auto);

    auto parsed = parser.parse_primitive_value_from_source(ValueType::Length, source, non_negative_range);
    if (!parsed)
        return nullptr;
    if (parsed->is_calculated()) {
        auto raw_length = parsed->as_calculated().resolve_raw_length({});
        if (raw_length.has_value() && !isfinite(*raw_length))
            return nullptr;
    }
    return parsed;
}

bool RustQueryParser::evaluate_supports_feature(void* context, FfiSupportsFeatureKind kind, FfiUtf16View ffi_source)
{
    VERIFY(context);
    VERIFY(!ffi_source.ascii);
    VERIFY(ffi_source.utf16 || ffi_source.length == 0);
    auto& parser = *static_cast<Parser*>(context);
    auto source = Utf16View { reinterpret_cast<char16_t const*>(ffi_source.utf16), ffi_source.length };
    switch (kind) {
    case FfiSupportsFeatureKind::Declaration: {
        Array contexts { RuleContext::SupportsCondition };
        auto items = RustSyntaxParser::parse_block_contents(parser, source, contexts, PreservePropertySourceText::Yes);
        if (items.size() != 1 || !items.first().has<Vector<Declaration>>())
            return false;
        auto const& declarations = items.first().get<Vector<Declaration>>();
        return declarations.size() == 1 && parser.convert_to_style_property(declarations.first()).has_value();
    }
    case FfiSupportsFeatureKind::Selector: {
        auto selectors = parse_selector_list_in_rust(source, parser.m_declared_namespaces, false, false);
        return selectors.has_value() && selectors->size() == 1
            && !selectors->first()->contains_unknown_webkit_pseudo_element();
    }
    case FfiSupportsFeatureKind::FontTech:
        return font_tech_is_supported(source);
    case FfiSupportsFeatureKind::FontFormat:
        return font_format_is_supported(source);
    case FfiSupportsFeatureKind::AtRule:
        return at_rule_is_supported(source);
    case FfiSupportsFeatureKind::Env:
        return environment_variable_from_string(source).has_value();
    }
    VERIFY_NOT_REACHED();
}

}
