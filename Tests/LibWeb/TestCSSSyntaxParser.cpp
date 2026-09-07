/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/SelectorRustFFI.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

static RustDeclarationBlock parse_native_declaration_block(Utf16View source)
{
    ValueParserFFI::ParseContext context {};
    u8 rule_context = to_underlying(RuleContext::Style);
    auto* parse = ValueParserFFI::rust_parse_css_block_syntax(
        { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &rule_context, 1, &context, false);
    auto* block = ValueParserFFI::rust_css_syntax_parse_declaration_block(parse);
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    return RustDeclarationBlock { block };
}

TEST_CASE(style_engine_consumes_native_declaration_blocks)
{
    auto source = u"*"sv;
    auto* parsed = SelectorFFI::rust_selector_parse(
        { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, nullptr, 0, false, false);
    VERIFY(parsed);
    Vector<uintptr_t> names;
    for (size_t index = 0; index < SelectorFFI::rust_parsed_selector_list_interned_name_count(parsed); ++index) {
        auto name = SelectorFFI::rust_parsed_selector_list_interned_name(parsed, index);
        names.append(Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(name.data), name.length }).to_raw_leaked());
    }
    auto* bound = SelectorFFI::rust_parsed_selector_list_bind_interned_names(parsed, names.data(), names.size());
    auto* selector = SelectorFFI::rust_bound_selector_list_selector(bound, 0);
    SelectorFFI::rust_bound_selector_list_destroy(bound);
    SelectorFFI::rust_parsed_selector_list_destroy(parsed);
    void const* selectors[] { selector };

    StyleEngine engine(StyleEngine::DeviceClass::ForegroundDesktop);
    auto sheet = engine.add_sheet(1, StyleEngineFFI::FfiCascadeOrigin::Author);
    auto rule = engine.add_style_rule(sheet, {}, selectors, {}, {}, {}, {});
    VERIFY(rule.value());
    SelectorFFI::rust_selector_destroy(selector);

    auto reused_values = [&]() -> u64 {
        for (size_t index = 0;; ++index) {
            StringView name;
            u64 value = 0;
            VERIFY(engine.counter(index, name, value));
            if (name == "specifiedValuesReused"sv)
                return value;
        }
    };
    auto first = parse_native_declaration_block(u"color: #14181c"sv);
    auto second = parse_native_declaration_block(u"color: rgb(20, 24, 28)"sv);
    auto transitions = parse_native_declaration_block(u"transition-duration: 1s"sv);
    auto retained = first.retain();
    StyleValueFFI::rust_style_ffi_counters_reset();
    engine.set_rule_declared_properties(rule, first);
    auto reused_before = reused_values();
    engine.set_rule_declared_properties(rule, second);
    EXPECT_EQ(reused_values(), reused_before + 1);
    EXPECT(!engine.css_transitions_may_observe_style_changes());
    first.replace(transitions);
    engine.set_rule_declared_properties(rule, retained);
    EXPECT(engine.css_transitions_may_observe_style_changes());
    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

TEST_CASE(style_engine_consumes_native_inline_declaration_blocks)
{
    StyleEngine engine(StyleEngine::DeviceClass::ForegroundDesktop);
    auto node = engine.allocate_style_node();
    auto declarations = parse_native_declaration_block(u"color: rgb(20, 24, 28); margin: var(--gap); --gap: 13px"sv);
    auto shared = declarations.share();
    auto transitions = parse_native_declaration_block(u"transition-duration: 1s"sv);
    auto retained = declarations.retain();
    StyleValueFFI::rust_style_ffi_counters_reset();
    engine.set_element_inline_style_properties(node, &declarations);
    EXPECT(!engine.css_transitions_may_observe_style_changes());
    engine.set_element_inline_style_properties(node, &shared);
    engine.set_element_inline_style_properties(node, nullptr);
    declarations.replace(transitions);
    engine.set_element_inline_style_properties(node, &retained);
    EXPECT(engine.css_transitions_may_observe_style_changes());
    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

TEST_CASE(style_engine_expands_presentation_hint_shorthands_in_rust)
{
    StyleEngine engine(StyleEngine::DeviceClass::ForegroundDesktop);
    auto node = engine.allocate_style_node();
    auto inherited = parse_native_declaration_block(u"color: inherit"sv);
    Vector<StyleProperty> hints { StyleProperty { Important::No, PropertyID::Border, inherited.properties()[0].value } };
    engine.set_element_presentational_hint_properties(node, StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint, hints);
    EXPECT(!engine.css_transitions_may_observe_style_changes());
    // Border expands through intermediate shorthands such as border-width. Each resulting
    // longhand after the first must reuse the same immutable keyword value. The 17 longhands
    // comprise four widths, four styles, four colors, and five border-image properties.
    for (size_t index = 0;; ++index) {
        StringView name;
        u64 value = 0;
        VERIFY(engine.counter(index, name, value));
        if (name == "specifiedValuesReused"sv) {
            EXPECT_EQ(value, 16ull);
            break;
        }
    }
    auto transitions = parse_native_declaration_block(u"transition-duration: 1s"sv);
    engine.set_element_presentational_hint_properties(node, StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint, transitions.properties());
    EXPECT(engine.css_transitions_may_observe_style_changes());
    engine.set_element_presentational_hint_properties(node, StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint, {});
}

TEST_CASE(native_declaration_block_specified_order_and_mutation)
{
    auto block = parse_native_declaration_block(u"margin: 2px !important; margin-left: 5px; width: 1px; width: 3px; --色: first; --other: second; --色: last"sv);
    EXPECT_EQ(block.properties().size(), 5u);
    EXPECT_EQ(block.properties()[0].property_id, PropertyID::MarginTop);
    EXPECT_EQ(block.properties()[3].property_id, PropertyID::MarginLeft);
    EXPECT_EQ(block.properties()[3].important, Important::Yes);
    EXPECT_EQ(block.properties()[3].value->to_utf16_string(SerializationMode::Normal), u"2px"sv);
    EXPECT_EQ(block.properties()[4].value->to_utf16_string(SerializationMode::Normal), u"3px"sv);
    EXPECT_EQ(block.custom_properties().size(), 2u);
    EXPECT_EQ(block.custom_properties().begin()->key, u"--色"sv);
    EXPECT_EQ(block.custom_properties().begin()->value.value->to_utf16_string(SerializationMode::Normal), u"last"sv);

    auto shared = block.share();
    auto value = block.properties()[4].value;
    EXPECT(block.set(PropertyID::MarginLeft, *value, Important::No));
    EXPECT(!block.set(PropertyID::MarginLeft, *value, Important::No));
    EXPECT_EQ(shared.properties()[3].important, Important::Yes);
    EXPECT_EQ(shared.properties()[3].value->to_utf16_string(SerializationMode::Normal), u"2px"sv);
    EXPECT(block.remove_custom("--色"_utf16_fly_string));
    EXPECT(!block.remove_custom("--色"_utf16_fly_string));
    EXPECT_EQ(shared.custom_properties().size(), 2u);

    auto logical = parse_native_declaration_block(u"margin-left: 1px; margin-inline-start: 2px"sv);
    auto original_value = logical.properties()[0].value;
    EXPECT(logical.set(PropertyID::MarginLeft, *original_value, Important::No));
    EXPECT_EQ(logical.properties()[0].property_id, PropertyID::MarginInlineStart);
    EXPECT_EQ(logical.properties()[1].property_id, PropertyID::MarginLeft);
}

TEST_CASE(native_declaration_block_merges_lists_without_expanding_again)
{
    auto block = parse_native_declaration_block(u"margin: var(--gap); color: red !important; --色: first; @media all { width: 999px; } margin-left: 7px; color: blue; --色: last"sv);
    EXPECT_EQ(block.properties().size(), 6u);
    EXPECT_EQ(block.properties()[0].property_id, PropertyID::Margin);
    EXPECT_EQ(block.properties()[4].property_id, PropertyID::Color);
    EXPECT_EQ(block.properties()[4].important, Important::Yes);
    EXPECT_EQ(block.properties()[5].property_id, PropertyID::MarginLeft);
    EXPECT_EQ(block.properties()[5].value->to_utf16_string(SerializationMode::Normal), u"7px"sv);
    EXPECT_EQ(block.custom_properties().size(), 1u);
    EXPECT_EQ(block.custom_properties().begin()->value.value->to_utf16_string(SerializationMode::Normal), u"last"sv);

    auto shared = block.share();
    EXPECT(block.remove(PropertyID::MarginLeft));
    EXPECT_EQ(block.properties().size(), 5u);
    EXPECT_EQ(shared.properties()[5].value->to_utf16_string(SerializationMode::Normal), u"7px"sv);
    EXPECT(parse_native_declaration_block(u"@media all { width: 999px; }"sv).is_empty());
    EXPECT(parse_native_declaration_block(u""sv).is_empty());
}

TEST_CASE(merge_native_declaration_blocks_on_a_worker)
{
    StyleValueFFI::rust_style_ffi_counters_reset();
    auto thread = Threading::Thread::construct("CSS block merge"sv, [] {
        auto source = u"color: red !important; --色: first; @unknown {} color: blue; --色: last"sv;
        ValueParserFFI::ParseContext context {};
        u8 rule_context = to_underlying(RuleContext::Style);
        auto* parse = ValueParserFFI::rust_parse_css_block_syntax(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &rule_context, 1, &context, false);
        auto* block = ValueParserFFI::rust_css_syntax_parse_declaration_block(parse);
        ValueParserFFI::rust_css_syntax_parse_free(parse);
        auto view = ValueParserFFI::rust_declaration_block_view(block);
        EXPECT_EQ(view.property_count, 1u);
        EXPECT_EQ(view.properties[0].property_id, to_underlying(PropertyID::Color));
        EXPECT(view.properties[0].important);
        EXPECT_EQ(view.custom_property_count, 1u);
        ValueParserFFI::rust_declaration_block_destroy(block);
        auto sheet_source = u"@page { @top-left { color: red !important; @unknown {} color: blue; } }"sv;
        auto* sheet = ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(sheet_source.utf16_span().data()), sheet_source.length_in_code_units() }, &context);
        auto data = ValueParserFFI::rust_css_syntax_parse_data(sheet);
        ValueParserFFI::FfiDeclarationBlock* margin = nullptr;
        for (auto const& rule : ReadonlySpan { data.rules, data.rule_count }) {
            if (rule.rule_kind == ValueParserFFI::FfiRuleKind::Margin)
                margin = ValueParserFFI::rust_declaration_block_from_data(rule.declaration_block);
        }
        VERIFY(margin);
        ValueParserFFI::rust_css_syntax_parse_free(sheet);
        view = ValueParserFFI::rust_declaration_block_view(margin);
        EXPECT_EQ(view.property_count, 1u);
        EXPECT(view.properties[0].important);
        ValueParserFFI::rust_declaration_block_destroy(margin);
        return 0;
    });
    thread->start();
    MUST(thread->join());
    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

TEST_CASE(native_declaration_block_preserves_unchanged_value_wrappers)
{
    auto block = parse_native_declaration_block(u"background-image: url(https://example.com/image.png); width: 3px"sv);
    auto image_value = block.properties()[0].value;
    auto width_value = block.properties()[1].value;
    block.set(PropertyID::Height, *width_value, Important::No);
    EXPECT_EQ(block.properties()[0].value.ptr(), image_value.ptr());
    block.remove(PropertyID::Width);
    EXPECT_EQ(block.properties()[0].value.ptr(), image_value.ptr());
}

TEST_CASE(retained_declaration_blocks_observe_mutations_and_replacement)
{
    auto block = parse_native_declaration_block(u"width: 13px; --色: green"sv);
    auto retained = block.retain();
    auto shared = block.share();
    auto identity = block.identity();
    EXPECT_EQ(retained.identity(), identity);
    EXPECT_NE(shared.identity(), identity);
    auto old_view = ValueParserFFI::rust_declaration_block_view(retained.handle());
    EXPECT_EQ(retained.properties().size(), 1u);
    EXPECT_EQ(retained.custom_properties().size(), 1u);
    auto revision = retained.revision();
    EXPECT(!block.set(PropertyID::Width, *retained.properties()[0].value, Important::No));
    EXPECT_EQ(retained.revision(), revision);

    EXPECT(block.remove(PropertyID::Width));
    EXPECT(retained.revision() > revision);
    // A retained handle's borrowed native view keeps its snapshot alive until refreshed.
    EXPECT_EQ(old_view.properties[0].property_id, to_underlying(PropertyID::Width));
    EXPECT_EQ(old_view.custom_properties[0].name.utf16[2], 0x8272u);
    EXPECT(retained.properties().is_empty());
    EXPECT_EQ(shared.properties().size(), 1u);
    revision = retained.revision();
    EXPECT(!block.remove(PropertyID::Width));
    EXPECT_EQ(retained.revision(), revision);

    retained.replace(parse_native_declaration_block(u"height: 7px; --色: blue"sv));
    EXPECT_EQ(block.identity(), identity);
    EXPECT_EQ(block.properties()[0].property_id, PropertyID::Height);
    EXPECT_EQ(block.custom_properties().begin()->value.value->to_utf16_string(SerializationMode::Normal), u"blue"sv);
    EXPECT_EQ(shared.custom_properties().begin()->value.value->to_utf16_string(SerializationMode::Normal), u"green"sv);

    // Releasing the original handle must not detach or destroy the retained owner.
    block = parse_native_declaration_block(u""sv);
    EXPECT_EQ(retained.identity(), identity);
    EXPECT_EQ(retained.properties()[0].property_id, PropertyID::Height);
}

TEST_CASE(retained_declaration_views_keep_replaced_data_alive)
{
    auto block = parse_native_declaration_block(u"width: 13px; --色: green"sv);
    auto retained = block.retain();
    auto view = ValueParserFFI::rust_declaration_block_view(retained.handle());
    block.replace(parse_native_declaration_block(u""sv));
    block = parse_native_declaration_block(u"height: 7px"sv);
    EXPECT(retained.is_empty());
    EXPECT_EQ(view.properties[0].property_id, to_underlying(PropertyID::Width));
    EXPECT_EQ(view.custom_properties[0].name.utf16[2], 0x8272u);
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(view.properties[0].value)));
    EXPECT_EQ(value->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    view = ValueParserFFI::rust_declaration_block_view(retained.handle());
    EXPECT_EQ(view.property_count, 0u);
    EXPECT_EQ(view.custom_property_count, 0u);
}

TEST_CASE(native_declaration_block_copy_on_write_on_another_thread)
{
    auto block = parse_native_declaration_block(u"width: 13px; --色: green"sv);
    auto* shared = ValueParserFFI::rust_declaration_block_share(block.handle());
    auto original = ValueParserFFI::rust_declaration_block_view(block.handle());
    auto thread = Threading::Thread::construct("CSS declarations"sv, [shared, original] {
        auto view = ValueParserFFI::rust_declaration_block_view(shared);
        EXPECT_EQ(view.custom_properties[0].name.utf16, original.custom_properties[0].name.utf16);
        auto declaration = view.properties[0];
        declaration.important = true;
        EXPECT(ValueParserFFI::rust_declaration_block_set(shared, &declaration));
        view = ValueParserFFI::rust_declaration_block_view(shared);
        EXPECT(ValueParserFFI::rust_declaration_block_remove_custom(shared, view.custom_properties[0].name));
        EXPECT_EQ(original.custom_property_count, 1u);
        EXPECT(!original.properties[0].important);
        view = ValueParserFFI::rust_declaration_block_view(shared);
        EXPECT(view.properties[0].important);
        EXPECT_EQ(view.custom_property_count, 0u);
        ValueParserFFI::rust_declaration_block_destroy(shared);
        return 0;
    });
    thread->start();
    MUST(thread->join());
    EXPECT_EQ(block.properties()[0].important, Important::No);
    EXPECT_EQ(block.custom_properties().size(), 1u);
}

TEST_CASE(build_native_declaration_block_without_host_callbacks)
{
    StyleValueFFI::rust_style_ffi_counters_reset();
    auto thread = Threading::Thread::construct("CSS declaration parse"sv, [] {
        auto source = u"a { margin: var(--gap) }"sv;
        ValueParserFFI::ParseContext context {};
        auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
        auto data = ValueParserFFI::rust_css_syntax_parse_data(parse);
        VERIFY(data.declaration_count == 1);
        auto* block = ValueParserFFI::rust_declaration_block_from_data(data.rules[data.roots[0]].declaration_block);
        ValueParserFFI::rust_css_syntax_parse_free(parse);
        auto view = ValueParserFFI::rust_declaration_block_view(block);
        EXPECT_EQ(view.property_count, 5u);
        EXPECT_EQ(view.properties[0].property_id, to_underlying(PropertyID::Margin));
        for (size_t index = 1; index < view.property_count; ++index) {
            auto* value = static_cast<StyleValueFFI::StyleValueData const*>(view.properties[index].value);
            EXPECT_EQ(value->tag, StyleValueFFI::StyleValueData::Tag::PendingSubstitution);
        }
        ValueParserFFI::rust_declaration_block_destroy(block);
        return 0;
    });
    thread->start();
    MUST(thread->join());
    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

TEST_CASE(nested_declaration_blocks_are_shared_native_worker_data)
{
    StyleValueFFI::rust_style_ffi_counters_reset();
    auto thread = Threading::Thread::construct("CSS nested declarations"sv, [] {
        auto source = u".parent { .child {} margin: var(--gap); --色: green; }"sv;
        ValueParserFFI::ParseContext context {};
        auto input = ValueParserFFI::FfiUtf16View { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() };
        auto* first = ValueParserFFI::rust_parse_css_stylesheet_syntax(input, &context);
        auto* second = ValueParserFFI::rust_parse_css_stylesheet_syntax(input, &context);
        auto first_data = ValueParserFFI::rust_css_syntax_parse_data(first);
        auto second_data = ValueParserFFI::rust_css_syntax_parse_data(second);
        size_t declaration_lists = 0;
        for (size_t index = 0; index < first_data.item_count; ++index) {
            auto const& item = first_data.items[index];
            if (item.item_type != 1)
                continue;
            ++declaration_lists;
            EXPECT_EQ(item.declaration_block, second_data.items[index].declaration_block);
            auto* block = ValueParserFFI::rust_declaration_block_from_data(item.declaration_block);
            auto* shared = ValueParserFFI::rust_declaration_block_share(block);
            auto view = ValueParserFFI::rust_declaration_block_view(block);
            EXPECT_EQ(view.property_count, 5u);
            EXPECT_EQ(view.custom_property_count, 1u);
            EXPECT(ValueParserFFI::rust_declaration_block_remove(block, to_underlying(PropertyID::MarginLeft)));
            EXPECT_EQ(ValueParserFFI::rust_declaration_block_view(shared).property_count, 5u);
            ValueParserFFI::rust_declaration_block_destroy(block);
            ValueParserFFI::rust_declaration_block_destroy(shared);
        }
        EXPECT_EQ(declaration_lists, 1u);
        ValueParserFFI::rust_css_syntax_parse_free(first);
        ValueParserFFI::rust_css_syntax_parse_free(second);
        return 0;
    });
    thread->start();
    MUST(thread->join());
    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

TEST_CASE(cached_stylesheets_share_native_declaration_blocks)
{
    auto source = u"a { margin: 13px; --色: green }"sv;
    ValueParserFFI::ParseContext context {};
    auto parse = [&] {
        return ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    };
    auto* first = parse();
    auto* second = parse();
    auto first_data = ValueParserFFI::rust_css_syntax_parse_data(first);
    auto second_data = ValueParserFFI::rust_css_syntax_parse_data(second);
    auto* original_block = first_data.rules[first_data.roots[0]].declaration_block;
    EXPECT_EQ(original_block, second_data.rules[second_data.roots[0]].declaration_block);
    RustDeclarationBlock first_block { ValueParserFFI::rust_declaration_block_from_data(original_block) };
    RustDeclarationBlock second_block { ValueParserFFI::rust_declaration_block_from_data(original_block) };
    EXPECT_EQ(first_block.properties().size(), 4u);
    EXPECT(first_block.remove(PropertyID::MarginTop));
    EXPECT(first_block.remove_custom("--色"_utf16_fly_string));
    EXPECT_EQ(second_block.properties().size(), 4u);
    EXPECT_EQ(second_block.custom_properties().size(), 1u);

    auto* third = parse();
    auto third_data = ValueParserFFI::rust_css_syntax_parse_data(third);
    EXPECT_EQ(third_data.rules[third_data.roots[0]].declaration_block, original_block);
    ValueParserFFI::rust_css_syntax_parse_free(first);
    ValueParserFFI::rust_css_syntax_parse_free(second);
    ValueParserFFI::rust_css_syntax_parse_free(third);
    EXPECT_EQ(second_block.properties()[0].value->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    EXPECT_EQ(second_block.custom_properties().begin()->value.value->to_utf16_string(SerializationMode::Normal), u"green"sv);
}

static void compare_parsed_syntax_serialization(Utf16View source, Utf16View expected)
{
    auto syntax = parse_as_syntax(source);
    EXPECT(syntax.has_value());
    if (syntax.has_value())
        EXPECT_EQ(syntax->serialize(), expected);
}

static void expect_serializations_equal(Utf16View lhs_source, Utf16View rhs_source)
{
    auto lhs = parse_as_syntax(lhs_source);
    auto rhs = parse_as_syntax(rhs_source);
    EXPECT(lhs.has_value());
    EXPECT(rhs.has_value());
    if (lhs.has_value() && rhs.has_value())
        EXPECT_EQ(lhs->serialize(), rhs->serialize());
}

TEST_CASE(single_universal)
{
    compare_parsed_syntax_serialization("*"_utf16, "*"_utf16);
}

TEST_CASE(single_ident)
{
    compare_parsed_syntax_serialization("thing"_utf16, "thing"_utf16);
}

TEST_CASE(single_type)
{
    for (auto type : { "angle"sv, "color"sv, "custom-ident"sv, "image"sv, "integer"sv, "length"sv,
             "length-percentage"sv, "number"sv, "percentage"sv, "resolution"sv, "string"sv, "time"sv,
             "url"sv, "transform-function"sv }) {
        auto source = Utf16String::formatted("<{}>", type);
        compare_parsed_syntax_serialization(source, source);
    }
}

TEST_CASE(multiple_keywords)
{
    compare_parsed_syntax_serialization("well|hello|friends"_utf16, "well | hello | friends"_utf16);
}

TEST_CASE(repeated_type)
{
    compare_parsed_syntax_serialization("<number>+"_utf16, "<number>+"_utf16);
}

TEST_CASE(repeated_with_commas)
{
    compare_parsed_syntax_serialization("<number>#"_utf16, "<number>#"_utf16);
}

TEST_CASE(complex)
{
    compare_parsed_syntax_serialization("well|<number>+|<string>#"_utf16, "well | <number>+ | <string>#"_utf16);
}

TEST_CASE(syntax_string)
{
    expect_serializations_equal("<number>"_utf16, "\"<number>\""_utf16);
    expect_serializations_equal("well | <number>+ | <string>#"_utf16, "\"well | <number>+ | <string>#\""_utf16);
}

TEST_CASE(invalid)
{
    for (auto source : { ""sv, " "sv, "<number"sv, "thing|"sv, "*|*"sv, "<transform-list>+"sv,
             "<transform-list>#"sv, "<woozle>"sv, "<number> <integer>"sv, "thingy whatsit"sv,
             "<number> +"sv, "<number> #"sv }) {
        EXPECT(!parse_as_syntax(Utf16String::from_utf8_without_validation(source)).has_value());
    }
}

TEST_CASE(devtools_declaration_metadata)
{
    auto source = u"COLOR: red !important; --custom: token stream; unknown-property: 1px; -webkit-unknown: 2px; -webkit-box-orient: horizontal; -webkit-box-orient: vertical; -webkit-box-orient: invalid; color: nonsense;"sv;

    auto declarations = parse_css_declaration_block_for_devtools(ParsingParams {}, source);
    EXPECT_EQ(declarations.size(), 8u);

    auto expect_declaration = [&](size_t index, Utf16View name, Utf16View value, Important important,
                                  bool is_custom_property, bool is_name_valid, bool is_valid) {
        EXPECT_EQ(declarations[index].name, name);
        EXPECT_EQ(declarations[index].value, value);
        EXPECT_EQ(declarations[index].important, important);
        EXPECT_EQ(declarations[index].is_custom_property, is_custom_property);
        EXPECT_EQ(declarations[index].is_name_valid, is_name_valid);
        EXPECT_EQ(declarations[index].is_valid, is_valid);
    };
    expect_declaration(0, u"COLOR"sv, u"red"sv, Important::Yes, false, true, true);
    expect_declaration(1, u"--custom"sv, u"token stream"sv, Important::No, true, true, true);
    expect_declaration(2, u"unknown-property"sv, u"1px"sv, Important::No, false, false, false);
    expect_declaration(3, u"-webkit-unknown"sv, u"2px"sv, Important::No, false, false, false);
    expect_declaration(4, u"-webkit-box-orient"sv, u"horizontal"sv, Important::No, false, true, true);
    expect_declaration(5, u"-webkit-box-orient"sv, u"vertical"sv, Important::No, false, true, true);
    expect_declaration(6, u"-webkit-box-orient"sv, u"invalid"sv, Important::No, false, true, false);
    expect_declaration(7, u"color"sv, u"nonsense"sv, Important::No, false, true, false);
}

TEST_CASE(retain_parsed_declarations_independently)
{
    auto source = u"a { width: 13px }"sv;
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
        { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    VERIFY(parse);
    auto data = ValueParserFFI::rust_css_syntax_parse_data(parse);
    EXPECT_EQ(data.declaration_count, 1u);
    auto* retained_parse = ValueParserFFI::rust_css_syntax_parse_retain(parse);
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    auto* value = static_cast<StyleValueFFI::StyleValueData const*>(data.declarations[0].parsed_value);
    VERIFY(value);
    auto* first = StyleValueFFI::rust_style_value_retain(value);
    auto* second = StyleValueFFI::rust_style_value_retain(value);
    StyleValueFFI::rust_style_value_release(first);
    EXPECT_EQ(StyleValueFFI::rust_style_value_computed_length_value(value), 13.0);
    ValueParserFFI::rust_css_syntax_parse_free(retained_parse);
    EXPECT_EQ(StyleValueFFI::rust_style_value_computed_length_value(second), 13.0);
    StyleValueFFI::rust_style_value_release(second);
}

TEST_CASE(publish_parse_result_after_worker_exit)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA ValueParserFFI::FfiSyntaxParse* parse = nullptr;
    auto thread = Threading::Thread::construct("CSS parser"sv, [&] {
        auto source = Utf16String::from_utf16(u".é😀 { width: 13px; --é😀: value }"sv);
        auto view = source.utf16_view();
        ValueParserFFI::ParseContext context {};
        parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(view.utf16_span().data()), view.length_in_code_units() }, &context);
        return 0;
    });
    thread->start();
    MUST(thread->join());

    VERIFY(parse);
    auto data = ValueParserFFI::rust_css_syntax_parse_data(parse);
    EXPECT_EQ(data.rule_count, 1u);
    EXPECT_EQ(data.declaration_count, 2u);
    VERIFY(data.rule_count == 1 && data.declaration_count == 2);
    EXPECT(data.rules[0].selector_list);
    auto const& declaration = data.declarations[1];
    EXPECT_EQ((Utf16View { reinterpret_cast<char16_t const*>(data.values + declaration.name_offset), declaration.name_length }), u"--é😀"sv);
    auto* value = static_cast<StyleValueFFI::StyleValueData const*>(data.declarations[0].parsed_value);
    VERIFY(value);
    EXPECT_EQ(StyleValueFFI::rust_style_value_computed_length_value(value), 13.0);
    ValueParserFFI::rust_css_syntax_parse_free(parse);
}

TEST_CASE(share_parsed_stylesheet_between_threads)
{
    auto source = u".é😀 { width: 13px } @supports (display: grid) { a { height: 7px } } @property --size { syntax: '<length>'; inherits: false; initial-value: 3px } @page :left { margin: 2px }"sv;
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
        { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    VERIFY(parse);
    auto data = ValueParserFFI::rust_css_syntax_parse_data(parse);
    VERIFY(data.declaration_count > 0);
    auto* expected_value = data.declarations[0].parsed_value;
    auto* expected_text = data.values;
    auto make_thread = [&] {
        auto* shared = ValueParserFFI::rust_css_syntax_parse_share(parse);
        return Threading::Thread::construct("CSS consumer"sv, [shared, expected_value, expected_text] {
            auto view = ValueParserFFI::rust_css_syntax_parse_data(shared);
            VERIFY(view.values == expected_text);
            VERIFY(view.declaration_count > 0);
            VERIFY(view.declarations[0].parsed_value == expected_value);
            VERIFY(StyleValueFFI::rust_style_value_computed_length_value(static_cast<StyleValueFFI::StyleValueData const*>(expected_value)) == 13.0);
            bool saw_selectors = false;
            bool saw_syntax = false;
            bool saw_page_selectors = false;
            bool saw_supports = false;
            for (size_t index = 0; index < view.rule_count; ++index) {
                auto const& rule = view.rules[index];
                if (rule.selector_list) {
                    auto* selectors = static_cast<SelectorFFI::RustParsedSelectorList const*>(rule.selector_list);
                    VERIFY(SelectorFFI::rust_parsed_selector_list_length(selectors) == 1);
                    saw_selectors = true;
                }
                if (rule.parsed_prelude_syntax) {
                    VERIFY(ValueParserFFI::rust_syntax_is_single_component(rule.parsed_prelude_syntax));
                    saw_syntax = true;
                }
                if (rule.page_selector_list) {
                    auto page_selectors = ValueParserFFI::rust_page_selector_list_data(rule.page_selector_list);
                    VERIFY(page_selectors.selector_count == 1);
                    saw_page_selectors = true;
                }
            }
            for (size_t index = 0; index < view.prelude_item_count; ++index) {
                if (auto* query = view.prelude_items[index].query) {
                    VERIFY(ValueParserFFI::css_query_evaluate_supports(static_cast<ValueParserFFI::FfiQueryHandle const*>(query)) == 1);
                    saw_supports = true;
                }
            }
            VERIFY(saw_selectors && saw_syntax && saw_page_selectors && saw_supports);
            auto second_view = ValueParserFFI::rust_css_syntax_parse_data(shared);
            VERIFY(view.declarations == second_view.declarations);
            VERIFY(view.rules == second_view.rules);
            ValueParserFFI::rust_css_syntax_parse_free(shared);
            return 0;
        });
    };
    auto first = make_thread();
    auto second = make_thread();
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    first->start();
    second->start();
    MUST(first->join());
    MUST(second->join());
}

TEST_CASE(cache_stylesheets_by_text_and_parsing_context)
{
    auto source = u".é😀 { width: 13px; background-image: url(image.png) }"sv;
    auto parse = [&](ValueParserFFI::ParseContext const& context) {
        return ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    };
    ValueParserFFI::ParseContext context {};
    auto* first = parse(context);
    auto* second = parse(context);
    auto data = ValueParserFFI::rust_css_syntax_parse_data(first);
    auto second_data = ValueParserFFI::rust_css_syntax_parse_data(second);
    EXPECT_EQ(data.values, second_data.values);
    EXPECT_NE(data.declarations, second_data.declarations);

    context.in_quirks_mode = true;
    auto* quirks = parse(context);
    EXPECT_NE(data.values, ValueParserFFI::rust_css_syntax_parse_data(quirks).values);
    context.in_quirks_mode = false;

    auto base_url = "https://example.com/directory/"sv;
    context.document_base_url = reinterpret_cast<u8 const*>(base_url.characters_without_null_termination());
    context.document_base_url_length = base_url.length();
    auto* with_base_url = parse(context);
    EXPECT_NE(data.values, ValueParserFFI::rust_css_syntax_parse_data(with_base_url).values);
    context.document_base_url = nullptr;
    context.document_base_url_length = 0;

    ComputedValuesFFI::FfiLengthResolutionContext lengths {};
    context.length_resolution_context = &lengths;
    auto* with_lengths = parse(context);
    lengths.viewport_width = 800;
    auto* resized = parse(context);
    EXPECT_NE(ValueParserFFI::rust_css_syntax_parse_data(with_lengths).values, ValueParserFFI::rust_css_syntax_parse_data(resized).values);
    bool resolved_viewport_length = false;
    lengths.resolved_viewport_relative_length = &resolved_viewport_length;
    auto* tracked = parse(context);
    auto* tracked_again = parse(context);
    EXPECT_NE(ValueParserFFI::rust_css_syntax_parse_data(tracked).values, ValueParserFFI::rust_css_syntax_parse_data(tracked_again).values);
    context.length_resolution_context = nullptr;

    size_t random_index = 7;
    context.random_function_index = &random_index;
    auto* with_counter = parse(context);
    auto final_index = random_index;
    random_index = 7;
    auto* with_counter_again = parse(context);
    EXPECT_EQ(random_index, final_index);
    EXPECT_EQ(ValueParserFFI::rust_css_syntax_parse_data(with_counter).values, ValueParserFFI::rust_css_syntax_parse_data(with_counter_again).values);

    for (auto* sheet : { first, second, quirks, with_base_url, with_lengths, resized, tracked, tracked_again, with_counter, with_counter_again })
        ValueParserFFI::rust_css_syntax_parse_free(sheet);
}

TEST_CASE(cache_stylesheets_across_native_string_representations)
{
    auto ascii = ".cached { width: 13px }"sv;
    auto utf16 = u".cached { width: 13px }"sv;
    ValueParserFFI::ParseContext context {};
    auto* first = ValueParserFFI::rust_parse_css_stylesheet_syntax(
        { reinterpret_cast<u8 const*>(ascii.characters_without_null_termination()), nullptr, ascii.length() }, &context);
    auto* second = ValueParserFFI::rust_parse_css_stylesheet_syntax(
        { nullptr, reinterpret_cast<u16 const*>(utf16.utf16_span().data()), utf16.length_in_code_units() }, &context);
    EXPECT_EQ(ValueParserFFI::rust_css_syntax_parse_data(first).values, ValueParserFFI::rust_css_syntax_parse_data(second).values);
    ValueParserFFI::rust_css_syntax_parse_free(first);
    ValueParserFFI::rust_css_syntax_parse_free(second);
}

TEST_CASE(stylesheet_cache_does_not_keep_graphs_alive)
{
    auto source = u".weak-cache-owner { width: 13px }"sv;
    ValueParserFFI::ParseContext context {};
    auto parse = [&] {
        return ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    };
    auto* first = parse();
    auto data = ValueParserFFI::rust_css_syntax_parse_data(first);
    VERIFY(data.declaration_count == 1);
    auto* retained_value = StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data.declarations[0].parsed_value));
    ValueParserFFI::rust_css_syntax_parse_free(first);

    auto* second = parse();
    auto second_data = ValueParserFFI::rust_css_syntax_parse_data(second);
    VERIFY(second_data.declaration_count == 1);
    // Keeping one value alive must not keep its entire originating parse in the cache.
    EXPECT_NE(second_data.declarations[0].parsed_value, retained_value);
    EXPECT_EQ(StyleValueFFI::rust_style_value_computed_length_value(retained_value), 13.0);
    StyleValueFFI::rust_style_value_release(retained_value);
    ValueParserFFI::rust_css_syntax_parse_free(second);
}

TEST_CASE(bind_parsed_selectors_independently)
{
    using namespace SelectorFFI;
    for (auto source : { u"DIV#test.item[data-name=\"value\"]"sv, u":is(.first, :not(#second))"sv,
             u":nth-child(2n+1 of .item)"sv, u"::slotted(.item)"sv, u"::part(label)"sv }) {
        auto* parsed = rust_selector_parse(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() },
            nullptr, 0, false, false);
        VERIFY(parsed);
        EXPECT_EQ(rust_parsed_selector_list_length(parsed), 1u);

        auto bind = [&] {
            Vector<uintptr_t> names;
            auto count = rust_parsed_selector_list_interned_name_count(parsed);
            for (size_t index = 0; index < count; ++index) {
                auto name = rust_parsed_selector_list_interned_name(parsed, index);
                names.append(Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(name.data), name.length }).to_raw_leaked());
            }
            auto* bound = rust_parsed_selector_list_bind_interned_names(parsed, names.data(), names.size());
            auto* selector = rust_bound_selector_list_selector(bound, 0);
            rust_bound_selector_list_destroy(bound);
            return selector;
        };

        auto* first = bind();
        auto* second = bind();
        rust_parsed_selector_list_destroy(parsed);
        auto first_text = rust_selector_serialize(first, false, nullptr, 0);
        auto second_text = rust_selector_serialize(second, false, nullptr, 0);
        EXPECT_EQ((Utf16View { reinterpret_cast<char16_t const*>(first_text.data), first_text.length }),
            (Utf16View { reinterpret_cast<char16_t const*>(second_text.data), second_text.length }));
        EXPECT_EQ(rust_selector_specificity(first), rust_selector_specificity(second));
        rust_selector_serialized_text_release(first_text.storage);
        rust_selector_serialized_text_release(second_text.storage);
        rust_selector_destroy(first);

        // The second binding must remain usable after both the source and first binding are gone.
        second_text = rust_selector_serialize(second, false, nullptr, 0);
        EXPECT(second_text.length > 0);
        rust_selector_serialized_text_release(second_text.storage);
        rust_selector_destroy(second);
    }
}

TEST_CASE(bind_worker_parsed_selectors)
{
    using namespace SelectorFFI;
    IGNORE_USE_IN_ESCAPING_LAMBDA RustParsedSelectorList* parsed = nullptr;
    auto parser = Threading::Thread::construct("CSS selectors"sv, [&] {
        auto source = u"é|DIV#😀:is(.é, :not([data-name='😀'])):nth-child(2n+1 of .item)::part(label)"sv;
        auto prefix = u"é"sv;
        SelectorFFI::StringView namespace_prefix { reinterpret_cast<u16 const*>(prefix.utf16_span().data()), prefix.length_in_code_units() };
        parsed = rust_selector_parse(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() },
            &namespace_prefix, 1, false, false);
        VERIFY(parsed);
        return 0;
    });
    parser->start();
    MUST(parser->join());

    auto name_count = rust_parsed_selector_list_interned_name_count(parsed);
    auto reader = Threading::Thread::construct("CSS selector reader"sv, [parsed, name_count] {
        for (u32 iteration = 0; iteration < 100; ++iteration) {
            VERIFY(rust_parsed_selector_list_length(parsed) == 1);
            VERIFY(rust_parsed_selector_list_interned_name_count(parsed) == name_count);
            for (size_t index = 0; index < name_count; ++index) {
                auto name = rust_parsed_selector_list_interned_name(parsed, index);
                VERIFY(name.length > 0);
                VERIFY(name.data);
            }
        }
        return 0;
    });
    reader->start();

    Vector<uintptr_t> names;
    for (size_t index = 0; index < name_count; ++index) {
        auto name = rust_parsed_selector_list_interned_name(parsed, index);
        names.append(Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(name.data), name.length }).to_raw_leaked());
    }
    auto* bound = rust_parsed_selector_list_bind_interned_names(parsed, names.data(), names.size());
    auto* selector = rust_bound_selector_list_selector(bound, 0);
    rust_bound_selector_list_destroy(bound);
    MUST(reader->join());

    auto destroyer = Threading::Thread::construct("CSS selector release"sv, [parsed] {
        rust_parsed_selector_list_destroy(parsed);
        return 0;
    });
    destroyer->start();
    MUST(destroyer->join());
    auto serialized = rust_selector_serialize(selector, false, nullptr, 0);
    EXPECT_EQ((Utf16View { reinterpret_cast<char16_t const*>(serialized.data), serialized.length }),
        u"é|DIV#😀:is(.é, :not([data-name=\"😀\"])):nth-child(2n+1 of .item)::part(label)"sv);
    rust_selector_serialized_text_release(serialized.storage);
    rust_selector_destroy(selector);
}

TEST_CASE(share_parsed_value_graph_between_threads)
{
    auto source = u"a { grid-template-columns: repeat(2, [é] minmax(10px, 1fr)); width: calc(1em + 2px); --custom: var(--é, [nested tokens]); content: '😀'; background-image: url(image.png) }"sv;
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
        { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    VERIFY(parse);
    auto data = ValueParserFFI::rust_css_syntax_parse_data(parse);
    VERIFY(data.declaration_count == 5);
    Vector<StyleValueFFI::StyleValueData const*> values;
    for (size_t index = 0; index < data.declaration_count; ++index) {
        auto* value = static_cast<StyleValueFFI::StyleValueData const*>(data.declarations[index].parsed_value);
        VERIFY(value);
        values.append(StyleValueFFI::rust_style_value_retain(value));
    }
    ValueParserFFI::rust_css_syntax_parse_free(parse);

    // Include a value constructed through the host boundary, whose strings must also be native.
    auto url = "image.png"_string;
    values.append(StyleValueFFI::rust_style_value_create_url(url.to_raw_leaked(), url.bytes().data(), url.bytes().size(), 0, nullptr, 0));
    StyleValueFFI::rust_style_ffi_counters_reset();

    auto make_thread = [&] {
        Vector<StyleValueFFI::StyleValueData const*> retained;
        for (auto* value : values)
            retained.append(StyleValueFFI::rust_style_value_retain(value));
        return Threading::Thread::construct("CSS values"sv, [retained = move(retained)] {
            for (auto* value : retained) {
                for (u32 iteration = 0; iteration < 100; ++iteration) {
                    auto* copy = StyleValueFFI::rust_style_value_retain(value);
                    VERIFY(StyleValueFFI::rust_style_value_equals(value, copy));
                    auto serialized = StyleValueFFI::rust_style_value_serialize(copy, 0);
                    VERIFY(serialized.has_value);
                    auto text = Utf16String::adopt_raw(serialized.raw);
                    VERIFY(!text.is_empty());
                    StyleValueFFI::rust_style_value_release(copy);
                }
                StyleValueFFI::rust_style_value_release(value);
            }
            return 0;
        });
    };
    auto first = make_thread();
    auto second = make_thread();
    for (auto* value : values)
        StyleValueFFI::rust_style_value_release(value);
    first->start();
    second->start();
    MUST(first->join());
    MUST(second->join());

    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

TEST_CASE(parse_strings_without_host_interning)
{
    StyleValueFFI::rust_style_ffi_counters_reset();
    IGNORE_USE_IN_ESCAPING_LAMBDA StyleValueFFI::StyleValueData const* retained_value = nullptr;
    auto thread = Threading::Thread::construct("CSS parser"sv, [&] {
        auto source = u"a { position-anchor: --shared-name; color: ReD; grid-template-columns: [shared-name] 1fr; content: 'text' }"sv;
        ValueParserFFI::ParseContext context {};
        auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
            { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
        VERIFY(parse);
        auto data = ValueParserFFI::rust_css_syntax_parse_data(parse);
        VERIFY(data.declaration_count == 4);
        for (size_t index = 0; index < data.declaration_count; ++index)
            VERIFY(data.declarations[index].parsed_value);
        retained_value = StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data.declarations[0].parsed_value));
        ValueParserFFI::rust_css_syntax_parse_free(parse);
        return 0;
    });
    thread->start();
    MUST(thread->join());

    VERIFY(retained_value);
    EXPECT_EQ(retained_value->tag, StyleValueFFI::StyleValueData::Tag::CustomIdent);
    auto view = StyleValueFFI::rust_css_string_view(&retained_value->custom_ident.custom_ident);
    EXPECT_EQ((Utf16View { reinterpret_cast<char16_t const*>(view.data), view.length }), u"--shared-name"sv);

    auto source = u"b { position-anchor: --shared-name }"sv;
    ValueParserFFI::ParseContext context {};
    auto* second_parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(
        { nullptr, reinterpret_cast<u16 const*>(source.utf16_span().data()), source.length_in_code_units() }, &context);
    VERIFY(second_parse);
    auto data = ValueParserFFI::rust_css_syntax_parse_data(second_parse);
    VERIFY(data.declaration_count == 1);
    auto* second_value = static_cast<StyleValueFFI::StyleValueData const*>(data.declarations[0].parsed_value);
    VERIFY(second_value);
    EXPECT(StyleValueFFI::rust_style_value_equals(retained_value, second_value));
    ValueParserFFI::rust_css_syntax_parse_free(second_parse);
    StyleValueFFI::rust_style_value_release(retained_value);

    for (size_t index = 0; index < StyleValueFFI::rust_style_ffi_counter_count(); ++index) {
        auto const* name_data = reinterpret_cast<char const*>(StyleValueFFI::rust_style_ffi_counter_name(index));
        auto name = StringView { name_data, strlen(name_data) };
        if (name == "stringRetainReleaseCallbacks"sv || name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 0u);
    }
}

}
