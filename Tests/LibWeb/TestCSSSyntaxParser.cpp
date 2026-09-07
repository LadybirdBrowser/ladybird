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
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/SelectorRustFFI.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

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
