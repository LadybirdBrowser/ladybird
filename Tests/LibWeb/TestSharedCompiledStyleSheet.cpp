/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/FontPlugin.h>

static GC::Ref<Web::DOM::Document> create_document()
{
    static Web::Platform::FontPlugin font_plugin { false };
    static bool font_plugin_installed = false;
    if (!font_plugin_installed) {
        Web::Platform::FontPlugin::install(font_plugin);
        font_plugin_installed = true;
    }
    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto* window = Web::HTML::window_from_global_object(realm->global_object());
    VERIFY(window);
    auto document = Web::HTML::HTMLDocument::create(Web::Bindings::principal_host_defined_page(*realm), *window);
    document->set_document_type(Web::DOM::Document::Type::HTML);
    document->set_window(*window);
    window->set_associated_document(document);
    auto root = MUST(document->create_element("html"_utf16, Web::Bindings::ElementCreationOptions {}));
    MUST(document->append_child(root));
    return document;
}

static GC::Ref<Web::DOM::Element> append_style(Web::DOM::Document& document, Utf16String text, Web::DOM::Node* parent = nullptr)
{
    auto style = MUST(document.create_element("style"_utf16, Web::Bindings::ElementCreationOptions {}));
    MUST(style->set_text_content(move(text)));
    MUST((parent ? parent : document.document_element())->append_child(style));
    return style;
}

TEST_CASE(unique_style_text_is_not_retained_in_the_shared_sheet_cache)
{
    auto document = create_document();
    for (size_t index = 0; index < 128; ++index) {
        auto style = append_style(document, Utf16String::formatted("/* {} */", index));
        style->remove();
    }
    EXPECT(document->style_computer().shared_compiled_style_sheets().is_empty());
}

TEST_CASE(shared_sheet_contents_are_released_after_the_last_detachment)
{
    auto document = create_document();
    auto second = append_style(document, "div { color: red; }"_utf16);
    auto host = MUST(document->create_element("div"_utf16, Web::Bindings::ElementCreationOptions {}));
    MUST(document->document_element()->append_child(host));
    auto shadow = MUST(host->attach_shadow({ .mode = Web::Bindings::ShadowRootMode::Open }));
    auto third = append_style(document, "div { color: red; }"_utf16, shadow.ptr());
    auto& cache = document->style_computer().shared_compiled_style_sheets();
    EXPECT_EQ(cache.size(), 1u);
    auto contents = cache.begin()->value->contents().make_weak_ptr();
    second->remove();
    EXPECT_EQ(cache.size(), 1u);
    EXPECT(contents);
    third->remove();
    EXPECT(cache.is_empty());
    EXPECT(!contents);
    auto replacement = append_style(document, "div { color: red; }"_utf16);
    EXPECT_EQ(cache.size(), 1u);
    replacement->remove();
    EXPECT(cache.is_empty());
}

TEST_CASE(shared_sheet_contents_are_released_after_copy_on_write)
{
    auto document = create_document();
    auto second = append_style(document, "div { color: blue; }"_utf16);
    auto& cache = document->style_computer().shared_compiled_style_sheets();
    EXPECT_EQ(cache.size(), 1u);
    auto contents = cache.begin()->value->contents().make_weak_ptr();
    MUST(document->style_scope().style_sheets().last()->insert_rule("span { color: red; }"_utf16, 0));
    EXPECT(cache.is_empty());
    EXPECT(!contents);
    second->remove();
}

TEST_CASE(first_instance_uses_shared_compiled_contents)
{
    auto document = create_document();
    auto first = append_style(document, "div { color: green; }"_utf16);
    auto& first_sheet = *document->style_scope().style_sheets().first();
    auto* shared = first_sheet.shared_compiled_style_sheet();
    EXPECT(shared);
    EXPECT_EQ(first_sheet.style_engine_sheet_id(), shared->sheet_id());
    EXPECT_EQ(shared->contents().style_engine_sheet_id(), shared->sheet_id());
    EXPECT_NE(&shared->contents(), &first_sheet);

    auto host = MUST(document->create_element("div"_utf16, Web::Bindings::ElementCreationOptions {}));
    MUST(document->document_element()->append_child(host));
    auto shadow = MUST(host->attach_shadow({ .mode = Web::Bindings::ShadowRootMode::Open }));
    auto second = append_style(document, "div { color: green; }"_utf16, shadow.ptr());
    auto& second_sheet = *shadow->style_scope().style_sheets().first();
    EXPECT_EQ(second_sheet.shared_compiled_style_sheet(), shared);
    EXPECT_EQ(second_sheet.style_engine_sheet_id(), first_sheet.style_engine_sheet_id());
    auto contents = shared->contents().make_weak_ptr();
    first->remove();
    EXPECT(contents);
    EXPECT_EQ(second_sheet.shared_compiled_style_sheet(), shared);
    second->remove();
    EXPECT(!contents);
    EXPECT(document->style_computer().shared_compiled_style_sheets().is_empty());
}

TEST_CASE(occurrences_share_across_media_and_disabled_changes)
{
    auto document = create_document();
    auto first = append_style(document, "div { color: purple; }"_utf16);
    auto second = append_style(document, "div { color: purple; }"_utf16);
    auto& first_sheet = *document->style_scope().style_sheets()[0];
    auto& second_sheet = *document->style_scope().style_sheets()[1];
    auto* shared = first_sheet.shared_compiled_style_sheet();
    EXPECT(shared);
    EXPECT_EQ(second_sheet.shared_compiled_style_sheet(), shared);
    auto sheet_id = first_sheet.style_engine_sheet_id();
    first_sheet.set_disabled(true);
    second_sheet.set_media("print"_utf16);
    EXPECT_EQ(first_sheet.shared_compiled_style_sheet(), shared);
    EXPECT_EQ(second_sheet.shared_compiled_style_sheet(), shared);
    EXPECT_EQ(first_sheet.style_engine_sheet_id(), sheet_id);
    EXPECT_EQ(second_sheet.style_engine_sheet_id(), sheet_id);
    first_sheet.set_disabled(false);
    second_sheet.set_media("screen"_utf16);
    EXPECT_EQ(first_sheet.style_engine_sheet_id(), sheet_id);
    EXPECT_EQ(second_sheet.style_engine_sheet_id(), sheet_id);
    MUST(first_sheet.insert_rule("span { color: red; }"_utf16, 0));
    EXPECT(!first_sheet.shared_compiled_style_sheet());
    EXPECT_EQ(second_sheet.shared_compiled_style_sheet(), shared);
    first->remove();
    EXPECT_EQ(document->style_computer().shared_compiled_style_sheets().size(), 1u);
    second->remove();
    EXPECT(document->style_computer().shared_compiled_style_sheets().is_empty());
}

TEST_CASE(anonymous_layers_keep_distinct_identities_in_the_same_scope)
{
    auto document = create_document();
    auto first = append_style(document, "@layer { div { color: green; } }"_utf16);
    auto second = append_style(document, "@layer { div { color: green; } }"_utf16);
    auto& first_sheet = *document->style_scope().style_sheets()[0];
    auto& second_sheet = *document->style_scope().style_sheets()[1];
    EXPECT(first_sheet.shared_compiled_style_sheet());
    EXPECT(!second_sheet.shared_compiled_style_sheet());
    EXPECT_NE(first_sheet.style_engine_sheet_id(), second_sheet.style_engine_sheet_id());
    first->remove();
    second->remove();
    EXPECT(document->style_computer().shared_compiled_style_sheets().is_empty());
}
