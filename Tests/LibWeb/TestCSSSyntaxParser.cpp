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
#include <LibWeb/CSS/RustDescriptorBlock.h>
#include <LibWeb/CSS/RustPageSelectors.h>
#include <LibWeb/CSS/RustRule.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/SelectorRustFFI.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

static RustDeclarationBlock parse_native_declaration_block(Utf16View);

TEST_CASE(native_import_resource_owner_retains_rules_without_cssom)
{
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(u"@import url('sheet.css') layer(外) screen;"sv), &context);
    RustRuleList roots { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    auto identity = roots.identity_at(0);
    auto import = StyleSheetImport::create(roots.at(0), nullptr);
    roots.clear();

    EXPECT_EQ(import->native_rule().identity(), identity);
    EXPECT_EQ(import->href(), "sheet.css"sv);
    EXPECT_EQ(import->native_media_list().media_text(), u"screen"sv);
    EXPECT_EQ(import->native_rule().internal_layer_name().value(), u"外"sv);
    EXPECT(!import->loaded_style_sheet());
    EXPECT(!import->parent_style_sheet());
    EXPECT_EQ(import->loading_state(), StyleSheetState::LoadingState::Unloaded);
}

TEST_CASE(native_rule_lists_own_live_payloads_and_child_order)
{
    StyleValueFFI::rust_style_ffi_counters_reset();
    IGNORE_USE_IN_ESCAPING_LAMBDA ValueParserFFI::NativeRuleList const* root = nullptr;
    auto thread = Threading::Thread::construct("CSS native rule owners"sv, [&] {
        ValueParserFFI::ParseContext context {};
        auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(u"@media all { .色 { width: 13px } } @page 原:left { margin-left: 17px }"sv), &context);
        root = ValueParserFFI::rust_css_syntax_native_rules(parse);
        ValueParserFFI::rust_css_syntax_parse_free(parse);
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

    auto* retained = ValueParserFFI::rust_rule_list_retain(root);
    RustRule media { ValueParserFFI::rust_rule_list_at(root, 0) };
    RustRule page { ValueParserFFI::rust_rule_list_at(root, 1) };
    EXPECT_EQ(media.type(), RustRule::Type::Media);
    EXPECT_NE(media.identity(), page.identity());
    auto* children = ValueParserFFI::rust_rule_children(media.handle());
    EXPECT_EQ(ValueParserFFI::rust_rule_list_count(children), 1u);
    RustRule style { ValueParserFFI::rust_rule_list_at(children, 0) };
    RustRule style_owner { style };
    auto declarations = style.declarations().release_value();
    EXPECT_EQ(declarations.identity(), style_owner.declarations()->identity());
    auto replacement = parse_native_declaration_block(u"width: 29px"sv);
    style_owner.declarations()->replace(replacement);
    EXPECT_EQ(declarations.properties()[0].value->to_utf16_string(SerializationMode::Normal), u"29px"sv);

    RustPageSelectors original_selectors { page.payload().page_selectors };
    auto replacement_selectors = RustPageSelectors::parse(u"新しい:right"sv).release_value();
    ValueParserFFI::rust_rule_set_page_selectors(page.handle(), replacement_selectors.handle());
    EXPECT_EQ(RustPageSelectors { page.payload().page_selectors }.serialize(), u"新しい:right"sv);
    EXPECT_EQ(original_selectors.serialize(), u"原:left"sv);

    ValueParserFFI::rust_rule_list_remove(root, 0);
    EXPECT_EQ(ValueParserFFI::rust_rule_list_count(retained), 1u);
    EXPECT_EQ(ValueParserFFI::rust_rule_identity(ValueParserFFI::rust_rule_list_at(retained, 0)), page.identity());
    ValueParserFFI::rust_rule_list_insert(root, 1, media.handle());
    EXPECT_EQ(ValueParserFFI::rust_rule_identity(ValueParserFFI::rust_rule_list_at(retained, 1)), media.identity());
    ValueParserFFI::rust_rule_list_release(root);
    ValueParserFFI::rust_rule_list_clear(retained);
    EXPECT_EQ(ValueParserFFI::rust_rule_list_count(children), 1u);
    ValueParserFFI::rust_rule_list_release(retained);
    EXPECT_EQ(style.declarations()->identity(), declarations.identity());
}

TEST_CASE(native_matching_selectors_follow_edits_and_reparenting)
{
    auto matching_text = [&](RustRule const& rule) {
        auto* bound = static_cast<SelectorFFI::RustBoundSelectorList*>(ValueParserFFI::rust_rule_matching_selectors(rule.handle()));
        EXPECT_EQ(SelectorFFI::rust_bound_selector_list_length(bound), 1u);
        auto* selector = SelectorFFI::rust_bound_selector_list_selector(bound, 0);
        SelectorFFI::rust_bound_selector_list_destroy(bound);
        auto serialized = SelectorFFI::rust_selector_serialize(selector, false, nullptr, 0);
        auto text = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(serialized.data), serialized.length });
        SelectorFFI::rust_selector_serialized_text_release(serialized.storage);
        SelectorFFI::rust_selector_destroy(selector);
        return text;
    };
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(u".親 { @media all { & > .子 {} } color: red; } @scope (.root) { @media all {} }"sv), &context);
    RustRuleList rules { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    RustRuleList other { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    auto parent = rules.at(0);
    auto* groups = ValueParserFFI::rust_rule_children(parent.handle());
    RustRuleList children { ValueParserFFI::rust_rule_list_retain(ValueParserFFI::rust_rule_children(ValueParserFFI::rust_rule_list_at(groups, 0))) };
    auto child = children.at(0);
    EXPECT_EQ(matching_text(child), u":is(.親) > .子"sv);
    EXPECT_EQ(matching_text(child), u":is(.親) > .子"sv);
    EXPECT(ValueParserFFI::rust_rule_set_selector_text(parent.handle(), ffi_utf16_view(u".新"sv), nullptr));
    EXPECT_EQ(matching_text(child), u":is(.新) > .子"sv);
    RustRule nested_declarations { ValueParserFFI::rust_rule_list_at(groups, 1) };
    EXPECT_EQ(matching_text(nested_declarations), u".新"sv);
    auto* other_groups = ValueParserFFI::rust_rule_children(other.at(0).handle());
    RustRule other_child { ValueParserFFI::rust_rule_list_at(ValueParserFFI::rust_rule_children(ValueParserFFI::rust_rule_list_at(other_groups, 0)), 0) };
    EXPECT_EQ(matching_text(other_child), u":is(.親) > .子"sv);

    children.remove(0);
    EXPECT_EQ(ValueParserFFI::rust_rule_nesting_parent_kind(child.handle()), ValueParserFFI::StyleNestingParent::None);
    EXPECT_EQ(matching_text(child), u":where(:scope) > .子"sv);
    RustRuleList scope_children { ValueParserFFI::rust_rule_list_retain(ValueParserFFI::rust_rule_children(rules.at(1).handle())) };
    scope_children.insert(1, child);
    EXPECT_EQ(ValueParserFFI::rust_rule_nesting_parent_kind(child.handle()), ValueParserFFI::StyleNestingParent::Scope);
    EXPECT_EQ(matching_text(child), u":where(:scope) > .子"sv);
    auto block = parse_native_declaration_block(u"width: 1px"sv);
    RustRule declarations { block };
    RustRuleList conditional_children { ValueParserFFI::rust_rule_list_retain(ValueParserFFI::rust_rule_children(scope_children.at(0).handle())) };
    conditional_children.insert(0, declarations);
    EXPECT_EQ(matching_text(declarations), u":where(:scope)"sv);
    rules.clear();
    EXPECT_EQ(ValueParserFFI::rust_rule_nesting_parent_kind(child.handle()), ValueParserFFI::StyleNestingParent::None);
}

TEST_CASE(native_scope_matching_preserves_presence_context_and_cache_lifetimes)
{
    auto take_text = [](void* result) {
        auto* bound = static_cast<SelectorFFI::RustBoundSelectorList*>(result);
        EXPECT_EQ(SelectorFFI::rust_bound_selector_list_length(bound), 1u);
        auto* selector = SelectorFFI::rust_bound_selector_list_selector(bound, 0);
        SelectorFFI::rust_bound_selector_list_destroy(bound);
        auto serialized = SelectorFFI::rust_selector_serialize(selector, false, nullptr, 0);
        auto text = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(serialized.data), serialized.length });
        SelectorFFI::rust_selector_serialized_text_release(serialized.storage);
        SelectorFFI::rust_selector_destroy(selector);
        return text;
    };
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(u"@import 'one.css' scope((.入口) to (.出口)); .親 { @scope (& .根) to (.限) {} @media all { @scope (& .内) {} } } @scope {} @scope to (.終) {} :has(.親) { @scope (:has(&)) {} }"sv), &context);
    RustRuleList rules { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    EXPECT_EQ(rules.size(), 5u);
    auto parent = rules.at(1);
    RustRuleList children { ValueParserFFI::rust_rule_list_retain(ValueParserFFI::rust_rule_children(parent.handle())) };
    Optional<RustRule> scope { children.at(0) };
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_start_selectors(scope->handle())), u":is(.親) .根"sv);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_end_selectors(scope->handle())), u":where(:scope) .限"sv);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_start_selectors(scope->handle())), u":is(.親) .根"sv);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_end_selectors(scope->handle())), u":where(:scope) .限"sv);
    EXPECT(ValueParserFFI::rust_rule_set_selector_text(parent.handle(), ffi_utf16_view(u".新"sv), nullptr));
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_start_selectors(scope->handle())), u":is(.新) .根"sv);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_end_selectors(scope->handle())), u":where(:scope) .限"sv);
    RustRule conditional_scope { ValueParserFFI::rust_rule_list_at(ValueParserFFI::rust_rule_children(children.at(1).handle()), 0) };
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_start_selectors(conditional_scope.handle())), u":where(:scope) .内"sv);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_start_selectors(rules.at(0).handle())), u".入口"sv);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_end_selectors(rules.at(0).handle())), u":where(:scope) .出口"sv);
    EXPECT(!ValueParserFFI::rust_rule_scope_start_selectors(rules.at(2).handle()));
    EXPECT(!ValueParserFFI::rust_rule_scope_end_selectors(rules.at(2).handle()));
    EXPECT(!ValueParserFFI::rust_rule_scope_start_selectors(rules.at(3).handle()));
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_end_selectors(rules.at(3).handle())), u":where(:scope) .終"sv);
    RustRule empty_scope { ValueParserFFI::rust_rule_list_at(ValueParserFFI::rust_rule_children(rules.at(4).handle()), 0) };
    auto* empty = static_cast<SelectorFFI::RustBoundSelectorList*>(ValueParserFFI::rust_rule_scope_start_selectors(empty_scope.handle()));
    VERIFY(empty);
    EXPECT_EQ(SelectorFFI::rust_bound_selector_list_length(empty), 0u);
    SelectorFFI::rust_bound_selector_list_destroy(empty);
    auto* retained_end = ValueParserFFI::rust_rule_scope_end_selectors(scope->handle());
    children.remove(0);
    EXPECT_EQ(take_text(ValueParserFFI::rust_rule_scope_start_selectors(scope->handle())), u":where(:scope) .根"sv);
    scope.clear();
    rules.clear();
    EXPECT_EQ(take_text(retained_end), u":where(:scope) .限"sv);
}

TEST_CASE(native_rule_factory_preserves_declaration_context_and_cached_data)
{
    auto source = u"@import 'sheet.css'; @layer 雪;\n.色 { width: 13px; @media all {} height: 17px; } @function --幅() { --長さ: 19px; @media all { result: var(--長さ); } } @page 原:left { @TOP-LEFT { width: 23px; } }"sv;
    ValueParserFFI::ParseContext context {};
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(source), &context);
    RustRuleList rules { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    auto original_layer = rules.at(1).payload().layer_names;
    ValueParserFFI::rust_css_syntax_parse_free(parse);

    // The native root, not a C++ parse wrapper, keeps the weak cache entry alive.
    auto* cached = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(source), &context);
    RustRuleList other { ValueParserFFI::rust_css_syntax_native_rules(cached) };
    ValueParserFFI::rust_css_syntax_parse_free(cached);
    EXPECT_EQ(other.at(1).payload().layer_names, original_layer);
    EXPECT_NE(rules.at(2).identity(), other.at(2).identity());
    EXPECT_EQ(rules.size(), 5u);

    auto style = rules.at(2);
    EXPECT(style.payload().has_source_position);
    auto* nested = ValueParserFFI::rust_rule_children(style.handle());
    EXPECT_EQ(ValueParserFFI::rust_rule_list_count(nested), 2u);
    RustRule declarations { ValueParserFFI::rust_rule_list_at(nested, 1) };
    EXPECT_EQ(declarations.type(), RustRule::Type::NestedDeclarations);
    EXPECT(declarations.payload().has_source_position);
    EXPECT_EQ(declarations.declarations()->properties()[0].value->to_utf16_string(SerializationMode::Normal), u"17px"sv);

    auto* function_children = ValueParserFFI::rust_rule_children(rules.at(3).handle());
    EXPECT_EQ(ValueParserFFI::rust_rule_type(ValueParserFFI::rust_rule_list_at(function_children, 0)), RustRule::Type::FunctionDeclarations);
    auto* conditional_children = ValueParserFFI::rust_rule_children(ValueParserFFI::rust_rule_list_at(function_children, 1));
    EXPECT_EQ(ValueParserFFI::rust_rule_type(ValueParserFFI::rust_rule_list_at(conditional_children, 0)), RustRule::Type::FunctionDeclarations);

    auto* margins = ValueParserFFI::rust_rule_children(rules.at(4).handle());
    EXPECT_EQ(ValueParserFFI::rust_rule_list_count(margins), 1u);
    RustRule margin { ValueParserFFI::rust_rule_list_at(margins, 0) };
    EXPECT_EQ(margin.type(), RustRule::Type::Margin);
    auto name = margin.payload().name;
    EXPECT_EQ((Utf16View { reinterpret_cast<char16_t const*>(name.utf16), name.length }), u"top-left"sv);

    auto replacement = parse_native_declaration_block(u"width: 29px"sv);
    style.declarations()->replace(replacement);
    EXPECT_EQ(other.at(2).declarations()->properties()[0].value->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    rules.remove_imports();
    EXPECT_EQ(rules.size(), 4u);
    EXPECT_EQ(other.size(), 5u);
    auto* original_list = rules.handle();
    rules.replace(other);
    EXPECT_EQ(rules.handle(), original_list);
    EXPECT_EQ(rules.at(2).identity(), other.at(2).identity());
    EXPECT_EQ(style.declarations()->properties()[0].value->to_utf16_string(SerializationMode::Normal), u"29px"sv);
}

TEST_CASE(native_function_fragments_preserve_declaration_context)
{
    ValueParserFFI::ParseContext context {};
    u8 rule_context = to_underlying(RuleContext::AtFunction);
    auto* parse = ValueParserFFI::rust_parse_css_rule_syntax(ffi_utf16_view(u"@supports (display: block) { result: 50px; }"sv), &rule_context, 1, true, &context);
    RustRuleList rules { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    EXPECT_EQ(rules.size(), 1u);
    auto* children = ValueParserFFI::rust_rule_children(rules.at(0).handle());
    EXPECT_EQ(ValueParserFFI::rust_rule_list_count(children), 1u);
    RustRule declarations { ValueParserFFI::rust_rule_list_at(children, 0) };
    EXPECT_EQ(declarations.type(), RustRule::Type::FunctionDeclarations);
    parse = ValueParserFFI::rust_parse_css_block_syntax(ffi_utf16_view(u"result: 60px;"sv), &rule_context, 1, &context, false);
    RustRuleList block { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    ValueParserFFI::rust_css_syntax_parse_free(parse);
    EXPECT_EQ(block.size(), 1u);
    EXPECT_EQ(block.at(0).type(), RustRule::Type::FunctionDeclarations);
}

TEST_CASE(native_rule_image_traversal_preserves_attachment_resources)
{
    ValueParserFFI::ParseContext context {};
    auto source = u"@import 'imported.css'; .根 { background-image: url(root.png); & .子 { content: url(child.png); } background-image: url(nested-declaration.png); } @media not all { .隠 { border-image-source: image-set(url(one.png) 1x, url(two.png) 2x); } } @supports (unknown: value) { .無 { background-image: url(unsupported.png); } } @keyframes 動 { from { background-image: url(frame.png); } }"sv;
    auto* parse = ValueParserFFI::rust_parse_css_stylesheet_syntax(ffi_utf16_view(source), &context);
    RustRuleList rules { ValueParserFFI::rust_css_syntax_native_rules(parse) };
    ValueParserFFI::rust_css_syntax_parse_free(parse);

    auto images = [&] {
        Vector<NonnullRefPtr<StyleValue const>> values;
        ValueParserFFI::rust_rule_list_visit_images(rules.handle(), &values, [](void* context, void const* value) {
            auto retained = StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(value));
            static_cast<Vector<NonnullRefPtr<StyleValue const>>*>(context)->append(StyleValue::adopt_rust_style_value_data(retained));
        });
        return values;
    };
    auto retained_images = images();
    EXPECT_EQ(retained_images.size(), 4u);
    EXPECT(retained_images[0]->is_image());
    EXPECT(retained_images[1]->is_image());
    EXPECT(retained_images[2]->is_image_set());
    EXPECT(retained_images[3]->is_image());

    rules.remove(1);
    EXPECT_EQ(images().size(), 2u);
    rules.clear();
    EXPECT(images().is_empty());
    EXPECT_EQ(retained_images[0]->to_utf16_string(SerializationMode::Normal), u"url(\"child.png\")"sv);
    EXPECT_EQ(retained_images[1]->to_utf16_string(SerializationMode::Normal), u"url(\"root.png\")"sv);
}

TEST_CASE(retained_descriptor_blocks_observe_replacement_and_keep_values_alive)
{
    auto values = parse_native_declaration_block(u"width: 13px; height: 29px"sv);
    auto name = DescriptorNameAndID::from_custom_name("--色"_utf16_fly_string);
    ValueParserFFI::FfiDescriptor descriptor { ffi_utf16_view(name.name()), to_underlying(name.id()), values.properties()[0].value->rust_style_value_data() };
    RustDescriptorBlock block { ValueParserFFI::rust_descriptor_block_create(&descriptor, 1) };
    auto retained = block.retain();
    auto shared = block.share();
    auto borrowed = retained.descriptor(name);
    EXPECT(!retained.set(name, *values.properties()[0].value));
    EXPECT(block.set(name, *values.properties()[1].value));
    EXPECT_EQ(borrowed->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    EXPECT_EQ(retained.descriptor(name)->to_utf16_string(SerializationMode::Normal), u"29px"sv);
    EXPECT_EQ(shared.descriptor(name)->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    borrowed = retained.descriptor(name);
    block.replace(shared);
    EXPECT_EQ(borrowed->to_utf16_string(SerializationMode::Normal), u"29px"sv);
    EXPECT_EQ(retained.descriptor(name)->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    EXPECT(retained.remove(name));
    EXPECT_EQ(block.size(), 0u);
    EXPECT_EQ(shared.size(), 1u);
    block.replace(shared);
    auto survivor = [&] {
        auto owner = shared.share();
        return owner.retain();
    }();
    EXPECT_EQ(survivor.descriptor(name)->to_utf16_string(SerializationMode::Normal), u"13px"sv);
    EXPECT_EQ(block.size(), 1u);
}

TEST_CASE(native_descriptor_blocks_preserve_order_and_copy_on_write)
{
    auto values = parse_native_declaration_block(u"width: 13px; height: 29px"sv);
    auto first_value = values.properties()[0].value;
    auto second_value = values.properties()[1].value;
    auto first_name = DescriptorNameAndID::from_custom_name(Utf16FlyString::from_utf16(u"--first"sv));
    auto custom = DescriptorNameAndID::from_custom_name(Utf16FlyString::from_utf16(u"--色"sv));
    auto name_view = [](Utf16View name) -> ValueParserFFI::FfiUtf16View {
        return { nullptr, reinterpret_cast<u16 const*>(name.utf16_span().data()), name.length_in_code_units() };
    };
    ValueParserFFI::FfiDescriptor descriptors[] {
        { name_view(u"--first"sv), to_underlying(DescriptorID::Custom), first_value->rust_style_value_data() },
        { name_view(u"--色"sv), to_underlying(DescriptorID::Custom), second_value->rust_style_value_data() },
    };
    RustDescriptorBlock block { ValueParserFFI::rust_descriptor_block_create(descriptors, 2) };
    auto shared = block.share();
    auto unchanged_value = block.descriptor(custom);
    EXPECT(!block.set(first_name, *first_value));
    EXPECT(block.set(first_name, *second_value));
    EXPECT(block.item(0) == first_name.name());
    EXPECT_EQ(block.descriptor(custom)->rust_style_value_data(), unchanged_value->rust_style_value_data());
    EXPECT_EQ(shared.property_value(first_name), u"13px"sv);
    EXPECT(block.remove(custom));
    EXPECT(!block.remove(custom));
    EXPECT_EQ(block.size(), 1u);
    EXPECT_EQ(shared.size(), 2u);
    EXPECT_EQ(shared.item(1), custom.name());
}

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
        if (name == "internUtf16FlyStringCallbacks"sv)
            EXPECT_EQ(StyleValueFFI::rust_style_ffi_counter_value(index), 2u);
        if (name == "stringRetainReleaseCallbacks"sv)
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
    EXPECT_EQ(retained.properties().size(), 1u);
    EXPECT_EQ(retained.custom_properties().size(), 1u);
    auto revision = retained.revision();
    EXPECT(!block.set(PropertyID::Width, *retained.properties()[0].value, Important::No));
    EXPECT_EQ(retained.revision(), revision);

    EXPECT(block.remove(PropertyID::Width));
    EXPECT(retained.revision() > revision);
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
    auto source = u"COLOR: red !important; --custom: token stream; unknown-property: 1px; -webkit-unknown: 2px; -webkit-box-orient: horizontal; -webkit-box-orient: vertical; -webkit-box-orient: invalid; color: nonsense; @media all { width: 99px; } .内側 { height: 99px; } --名前: '😀'; color: blue; width: calc(1px + 2px);"sv;

    auto declarations = parse_css_declaration_block_for_devtools(ParsingParams {}, source);
    EXPECT_EQ(declarations.size(), 11u);

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
    expect_declaration(8, u"--名前"sv, u"'😀'"sv, Important::No, true, true, true);
    expect_declaration(9, u"color"sv, u"blue"sv, Important::No, false, true, true);
    expect_declaration(10, u"width"sv, u"calc(1px + 2px)"sv, Important::No, false, true, true);
}

}
