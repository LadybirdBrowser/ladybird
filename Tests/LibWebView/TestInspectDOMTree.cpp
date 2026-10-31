/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

// The DOM tree that DevTools inspects takes in the document of each nested navigable in the same process, and reads
// its layout. A nested document whose layout is stale when the tree is requested must not crash WebContent.

static JsonObject const* find_element(JsonObject const& node, StringView name)
{
    if (auto node_name = node.get_string("name"sv); node_name.has_value() && *node_name == name)
        return &node;
    auto children = node.get_array("children"sv);
    if (!children.has_value())
        return nullptr;
    for (auto const& child : children->values()) {
        if (!child.is_object())
            continue;
        if (auto const* element = find_element(child.as_object(), name))
            return element;
    }
    return nullptr;
}

// The counter's value, as the tree shows it in the iframe's own document (not in the srcdoc attribute that holds it).
static Optional<u64> counter_in_iframe_document(JsonObject const& tree)
{
    auto const* iframe = find_element(tree, "IFRAME"sv);
    if (!iframe)
        return {};
    auto iframe_children = iframe->get_array("children"sv);
    if (!iframe_children.has_value() || iframe_children->size() != 1 || !iframe_children->at(0).is_object())
        return {};
    auto const& content_document = iframe_children->at(0).as_object();
    if (content_document.get_string("type"sv) != "document"sv)
        return {};
    auto const* counter = find_element(content_document, "SPAN"sv);
    if (!counter)
        return {};
    auto counter_children = counter->get_array("children"sv);
    if (!counter_children.has_value() || counter_children->size() != 1 || !counter_children->at(0).is_object())
        return {};
    auto text = counter_children->at(0).as_object().get_string("text"sv);
    if (!text.has_value())
        return {};
    return text->to_number<u64>();
}

TEST_CASE(inspect_dom_tree_with_stale_nested_layout)
{
    Test::HeadlessTestEnvironment environment { "TestInspectDOMTree"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    bool crashed = false;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    // The iframe's text changes every millisecond, so its document's layout is stale nearly all the time.
    view->load_html(R"html(<!DOCTYPE html>
<iframe srcdoc="<span id=counter>0</span>" onload="
    let n = 0;
    const counter = this.contentDocument.getElementById('counter');
    setInterval(() => counter.textContent = ++n, 1);
"></iframe>)html"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2 || crashed; });
    VERIFY(!crashed);

    size_t trees_received = 0;
    Optional<u64> last_counter;
    view->on_received_dom_tree = [&](JsonObject tree) {
        last_counter = counter_in_iframe_document(tree);
        VERIFY(last_counter.has_value());
        ++trees_received;
    };

    static constexpr size_t tree_requests = 50;
    for (size_t request = 1; request <= tree_requests; ++request) {
        view->inspect_dom_tree();
        Core::EventLoop::current().spin_until([&]() { return trees_received >= request || crashed; });
        VERIFY(!crashed);
    }

    // The tree shows the iframe's document as it is by then, after its timer has updated the counter.
    VERIFY(*last_counter > 0);

    outln("PASS: The DOM tree of a page whose nested document's layout is stale is inspected without a crash");
}
