/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibDevTools/Connection.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibHTTP/Header.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWebView/Attribute.h>
#include <LibWebView/ConsoleOutput.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Mutation.h>

using namespace AK::TimeLiterals;

static void pump(Core::EventLoop& loop)
{
    (void)loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}

static void spin_until(Core::EventLoop& loop, Function<bool()> condition, AK::Duration timeout = 2000_ms)
{
    for (i64 elapsed_ms = 0; elapsed_ms < timeout.to_milliseconds(); elapsed_ms += 5) {
        pump(loop);
        if (condition())
            return;
        MUST(Core::System::sleep_ms(5));
    }
    FAIL("Timed out waiting for condition");
}

static JsonObject make_node(Web::UniqueNodeID id, StringView type, StringView name)
{
    JsonObject node;
    node.set("id"sv, id.value());
    node.set("type"sv, type);
    node.set("name"sv, name);
    return node;
}

static JsonObject make_dom_tree()
{
    JsonObject target = make_node(4, "element"sv, "DIV"sv);
    target.set("visible"sv, true);
    target.set("scrollable"sv, true);
    target.set("display"sv, "grid"sv);

    JsonObject target_attributes;
    target_attributes.set("id"sv, "target"sv);
    target_attributes.set("class"sv, "fixture"sv);
    target.set("attributes"sv, move(target_attributes));

    JsonObject text = make_node(5, "text"sv, "#text"sv);
    text.set("text"sv, "Hello"sv);

    JsonObject comment = make_node(6, "comment"sv, "#comment"sv);
    comment.set("data"sv, "A comment"sv);

    JsonObject whitespace = make_node(7, "text"sv, "#text"sv);
    whitespace.set("text"sv, "   \n  "sv);

    JsonObject before = make_node(40, "pseudo-element"sv, "::before"sv);
    before.set("parent-id"sv, 4);
    before.set("pseudo-element"sv, to_underlying(Web::CSS::PseudoElement::Before));

    JsonObject subgrid = make_node(9, "element"sv, "SECTION"sv);
    subgrid.set("visible"sv, true);
    subgrid.set("display"sv, "grid"sv);

    JsonArray target_children;
    target_children.must_append(move(text));
    target_children.must_append(move(comment));
    target_children.must_append(move(whitespace));
    target_children.must_append(move(before));
    target_children.must_append(move(subgrid));
    target.set("children"sv, move(target_children));

    JsonObject sibling = make_node(8, "element"sv, "SPAN"sv);
    sibling.set("visible"sv, true);

    JsonObject first_flex_item = make_node(11, "element"sv, "P"sv);
    first_flex_item.set("visible"sv, true);

    JsonObject second_flex_item = make_node(12, "element"sv, "NAV"sv);
    second_flex_item.set("visible"sv, true);

    JsonArray flex_children;
    flex_children.must_append(move(first_flex_item));
    flex_children.must_append(move(second_flex_item));

    JsonObject flex = make_node(10, "element"sv, "ASIDE"sv);
    flex.set("visible"sv, true);
    flex.set("display"sv, "flex"sv);
    flex.set("children"sv, move(flex_children));

    JsonArray body_children;
    body_children.must_append(move(sibling));
    body_children.must_append(move(target));
    body_children.must_append(move(flex));

    JsonObject body = make_node(3, "element"sv, "BODY"sv);
    body.set("visible"sv, true);
    body.set("children"sv, move(body_children));

    JsonArray html_children;
    html_children.must_append(move(body));

    JsonObject html = make_node(2, "element"sv, "HTML"sv);
    html.set("visible"sv, true);
    html.set("children"sv, move(html_children));

    JsonArray document_children;
    document_children.must_append(move(html));

    JsonObject document = make_node(1, "document"sv, "#document"sv);
    document.set("children"sv, move(document_children));
    return document;
}

static JsonObject make_navigation_dom_tree()
{
    JsonObject heading = make_node(105, "element"sv, "H1"sv);
    heading.set("visible"sv, true);

    JsonObject main = make_node(104, "element"sv, "MAIN"sv);
    main.set("visible"sv, true);

    JsonArray main_children;
    main_children.must_append(move(heading));
    main.set("children"sv, move(main_children));

    JsonArray body_children;
    body_children.must_append(move(main));

    JsonObject body = make_node(103, "element"sv, "BODY"sv);
    body.set("visible"sv, true);
    body.set("children"sv, move(body_children));

    JsonArray html_children;
    html_children.must_append(move(body));

    JsonObject html = make_node(102, "element"sv, "HTML"sv);
    html.set("visible"sv, true);
    html.set("children"sv, move(html_children));

    JsonArray document_children;
    document_children.must_append(move(html));

    JsonObject document = make_node(101, "document"sv, "#document"sv);
    document.set("children"sv, move(document_children));
    return document;
}

static JsonObject make_accessibility_tree()
{
    JsonObject button = make_node(4, "element"sv, "Target button"sv);
    button.set("role"sv, "pushbutton"sv);

    JsonArray children;
    children.must_append(move(button));

    JsonObject root = make_node(1, "document"sv, "Document"sv);
    root.set("role"sv, "document"sv);
    root.set("children"sv, move(children));
    return root;
}

static WebView::DOMNodeProperties make_computed_style()
{
    JsonObject properties;
    properties.set("display"sv, "block"sv);
    properties.set("color"sv, "rgb(1, 2, 3)"sv);
    return { WebView::DOMNodeProperties::Type::ComputedStyle, move(properties) };
}

static JsonObject serialized_fixture_style_sheet();
static JsonObject serialized_fixture_user_agent_style_sheet();

static WebView::DOMNodeProperties make_applied_style_rules()
{
    JsonArray entries;

    JsonArray inline_declarations;
    JsonObject inline_display;
    inline_display.set("name"sv, "display"sv);
    inline_display.set("value"sv, "grid"sv);
    inline_display.set("priority"sv, ""sv);
    inline_display.set("isCustomProperty"sv, false);
    inline_display.set("isNameValid"sv, true);
    inline_display.set("isValid"sv, true);
    inline_display.set("inherits"sv, false);
    inline_declarations.must_append(move(inline_display));

    JsonObject inline_rule;
    inline_rule.set("type"sv, 100);
    inline_rule.set("className"sv, 100);
    inline_rule.set("cssText"sv, "display: grid;"sv);
    inline_rule.set("authoredText"sv, "display: grid;"sv);
    inline_rule.set("declarations"sv, move(inline_declarations));

    JsonObject inline_entry;
    inline_entry.set("rule"sv, move(inline_rule));
    inline_entry.set("isSystem"sv, false);
    inline_entry.set("matchedSelectorIndexes"sv, JsonArray {});
    inline_entry.set("inherited"sv, JsonValue {});
    entries.must_append(move(inline_entry));

    JsonArray rule_declarations;
    JsonObject rule_color;
    rule_color.set("name"sv, "color"sv);
    rule_color.set("value"sv, "rgb(1, 2, 3)"sv);
    rule_color.set("priority"sv, ""sv);
    rule_color.set("isCustomProperty"sv, false);
    rule_color.set("isNameValid"sv, true);
    rule_color.set("isValid"sv, true);
    rule_color.set("inherits"sv, true);
    rule_declarations.must_append(move(rule_color));

    JsonObject rule_invalid_color;
    rule_invalid_color.set("name"sv, "color"sv);
    rule_invalid_color.set("value"sv, "invalid-value"sv);
    rule_invalid_color.set("priority"sv, ""sv);
    rule_invalid_color.set("isCustomProperty"sv, false);
    rule_invalid_color.set("isNameValid"sv, true);
    rule_invalid_color.set("isValid"sv, false);
    rule_invalid_color.set("inherits"sv, true);
    rule_declarations.must_append(move(rule_invalid_color));

    JsonArray selectors;
    selectors.must_append("body div.fixture"sv);

    JsonArray selector_specificities;
    selector_specificities.must_append(0x101u);

    JsonArray matched_selector_indexes;
    matched_selector_indexes.must_append(0u);

    JsonObject rule;
    rule.set("type"sv, 1);
    rule.set("className"sv, "CSSStyleRule"sv);
    rule.set("selectors"sv, move(selectors));
    rule.set("selectorsSpecificity"sv, move(selector_specificities));
    rule.set("cssText"sv, "body div.fixture { color: rgb(1, 2, 3); }"sv);
    rule.set("authoredText"sv, "color: rgb(1, 2, 3);"sv);
    rule.set("declarations"sv, move(rule_declarations));
    rule.set("line"sv, 4);
    rule.set("column"sv, 9);
    rule.set("styleSheet"sv, serialized_fixture_style_sheet());

    JsonObject rule_entry;
    rule_entry.set("rule"sv, move(rule));
    rule_entry.set("isSystem"sv, false);
    rule_entry.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    rule_entry.set("inheritedNodeId"sv, 1u);
    entries.must_append(move(rule_entry));

    JsonArray user_agent_declarations;
    JsonObject user_agent_display;
    user_agent_display.set("name"sv, "display"sv);
    user_agent_display.set("value"sv, "block"sv);
    user_agent_display.set("priority"sv, ""sv);
    user_agent_display.set("isCustomProperty"sv, false);
    user_agent_display.set("isNameValid"sv, true);
    user_agent_display.set("isValid"sv, true);
    user_agent_display.set("inherits"sv, false);
    user_agent_declarations.must_append(move(user_agent_display));

    JsonArray user_agent_selectors;
    user_agent_selectors.must_append("div"sv);

    JsonArray user_agent_selector_specificities;
    user_agent_selector_specificities.must_append(0x1u);

    JsonArray user_agent_matched_selector_indexes;
    user_agent_matched_selector_indexes.must_append(0u);

    JsonObject user_agent_rule;
    user_agent_rule.set("type"sv, 1);
    user_agent_rule.set("className"sv, "CSSStyleRule"sv);
    user_agent_rule.set("selectors"sv, move(user_agent_selectors));
    user_agent_rule.set("selectorsSpecificity"sv, move(user_agent_selector_specificities));
    user_agent_rule.set("cssText"sv, "div { display: block; }"sv);
    user_agent_rule.set("authoredText"sv, "display: block;"sv);
    user_agent_rule.set("declarations"sv, move(user_agent_declarations));
    user_agent_rule.set("isSystem"sv, true);
    user_agent_rule.set("line"sv, 12);
    user_agent_rule.set("column"sv, 5);
    user_agent_rule.set("styleSheet"sv, serialized_fixture_user_agent_style_sheet());

    JsonObject user_agent_rule_entry;
    user_agent_rule_entry.set("rule"sv, move(user_agent_rule));
    user_agent_rule_entry.set("isSystem"sv, true);
    user_agent_rule_entry.set("matchedSelectorIndexes"sv, move(user_agent_matched_selector_indexes));
    user_agent_rule_entry.set("inherited"sv, JsonValue {});
    entries.must_append(move(user_agent_rule_entry));

    return { WebView::DOMNodeProperties::Type::AppliedStyleRules, move(entries) };
}

static WebView::DOMNodeProperties make_layout()
{
    JsonObject properties;
    properties.set("width"sv, "100px"sv);
    properties.set("height"sv, "50px"sv);
    properties.set("border-top-width"sv, 1);
    properties.set("border-right-width"sv, 2);
    properties.set("border-bottom-width"sv, 3);
    properties.set("border-left-width"sv, 4);
    properties.set("margin-top"sv, 5);
    properties.set("margin-right"sv, 6);
    properties.set("margin-bottom"sv, 7);
    properties.set("margin-left"sv, 8);
    properties.set("padding-top"sv, 9);
    properties.set("padding-right"sv, 10);
    properties.set("padding-bottom"sv, 11);
    properties.set("padding-left"sv, 12);
    properties.set("box-sizing"sv, "border-box"sv);
    properties.set("display"sv, "block"sv);
    properties.set("float"sv, "none"sv);
    properties.set("line-height"sv, "normal"sv);
    properties.set("position"sv, "static"sv);
    properties.set("z-index"sv, "auto"sv);
    return { WebView::DOMNodeProperties::Type::Layout, move(properties) };
}

static WebView::DOMNodeProperties make_used_fonts()
{
    JsonArray fonts;
    JsonObject font;
    font.set("name"sv, "Test Sans"sv);
    font.set("weight"sv, 400);
    fonts.must_append(move(font));
    return { WebView::DOMNodeProperties::Type::UsedFonts, move(fonts) };
}

static Web::CSS::StyleSheetIdentifier fixture_style_sheet()
{
    return { .type = Web::CSS::StyleSheetIdentifier::Type::StyleElement,
        .dom_element_unique_id = 9,
        .url = "https://example.test/style.css"_string,
        .rule_count = 2 };
}

static JsonObject serialized_fixture_style_sheet()
{
    JsonObject style_sheet;
    style_sheet.set("type"sv, Web::CSS::style_sheet_identifier_type_to_string(Web::CSS::StyleSheetIdentifier::Type::StyleElement));
    style_sheet.set("domElementUniqueId"sv, 9);
    style_sheet.set("url"sv, "https://example.test/style.css"sv);
    style_sheet.set("ruleCount"sv, 2);
    return style_sheet;
}

static Web::CSS::StyleSheetIdentifier fixture_user_agent_style_sheet()
{
    return { .type = Web::CSS::StyleSheetIdentifier::Type::UserAgent,
        .url = "CSS/Default.css"_string,
        .rule_count = 4 };
}

static JsonObject serialized_fixture_user_agent_style_sheet()
{
    JsonObject style_sheet;
    style_sheet.set("type"sv, Web::CSS::style_sheet_identifier_type_to_string(Web::CSS::StyleSheetIdentifier::Type::UserAgent));
    style_sheet.set("url"sv, "CSS/Default.css"sv);
    style_sheet.set("ruleCount"sv, 4);
    return style_sheet;
}

static JsonObject make_grid_line(double start, i32 number, i32 negative_number)
{
    JsonObject line;
    line.set("breadth"sv, 0);
    line.set("names"sv, JsonArray {});
    line.set("negativeNumber"sv, negative_number);
    line.set("number"sv, number);
    line.set("start"sv, start);
    line.set("type"sv, "explicit"sv);
    return line;
}

static JsonObject make_grid_track(double start, double breadth)
{
    JsonObject track;
    track.set("breadth"sv, breadth);
    track.set("start"sv, start);
    track.set("state"sv, "static"sv);
    track.set("type"sv, "explicit"sv);
    return track;
}

static JsonObject make_grid_dimension()
{
    JsonArray lines;
    lines.must_append(make_grid_line(0, 1, -2));
    lines.must_append(make_grid_line(100, 2, -1));

    JsonArray tracks;
    tracks.must_append(make_grid_track(0, 100));

    JsonObject dimension;
    dimension.set("lines"sv, move(lines));
    dimension.set("tracks"sv, move(tracks));
    return dimension;
}

static JsonObject make_grid_layout(Web::UniqueNodeID container_node_id, StringView area_name, bool is_subgrid = false)
{
    JsonObject area;
    area.set("columnEnd"sv, 2);
    area.set("columnStart"sv, 1);
    area.set("name"sv, area_name);
    area.set("rowEnd"sv, 2);
    area.set("rowStart"sv, 1);
    area.set("type"sv, "explicit"sv);

    JsonArray areas;
    areas.must_append(move(area));

    JsonObject fragment;
    fragment.set("areas"sv, move(areas));
    fragment.set("cols"sv, make_grid_dimension());
    fragment.set("rows"sv, make_grid_dimension());

    JsonArray fragments;
    fragments.must_append(move(fragment));

    JsonObject layout;
    layout.set("containerNodeId"sv, container_node_id.value());
    layout.set("direction"sv, "ltr"sv);
    layout.set("gridFragments"sv, move(fragments));
    layout.set("isSubgrid"sv, is_subgrid);
    layout.set("writingMode"sv, "horizontal-tb"sv);
    return layout;
}

static JsonArray make_grid_layouts()
{
    JsonArray grids;
    grids.must_append(make_grid_layout(Web::UniqueNodeID { 4 }, "content"sv));
    grids.must_append(make_grid_layout(Web::UniqueNodeID { 9 }, "subgrid"sv, true));
    grids.must_append(make_grid_layout(Web::UniqueNodeID { 8 }, "subframe"sv));
    return grids;
}

static JsonObject make_flex_item(Web::UniqueNodeID node_id, double main_base_size, double main_delta_size, StringView line_growth_state, StringView clamp_state)
{
    JsonObject sizing;
    sizing.set("clampState"sv, clamp_state);
    sizing.set("crossAxisDirection"sv, "vertical-tb"sv);
    sizing.set("crossMaxSize"sv, 20);
    sizing.set("crossMinSize"sv, 0);
    sizing.set("lineGrowthState"sv, line_growth_state);
    sizing.set("mainAxisDirection"sv, "horizontal-lr"sv);
    sizing.set("mainBaseSize"sv, main_base_size);
    sizing.set("mainDeltaSize"sv, main_delta_size);
    sizing.set("mainMaxSize"sv, 100);
    sizing.set("mainMinSize"sv, 10);

    JsonObject properties;
    properties.set("flex-basis"sv, "auto"sv);
    properties.set("flex-grow"sv, 1);
    properties.set("flex-shrink"sv, 1);
    properties.set("width"sv, "auto"sv);
    properties.set("min-width"sv, "auto"sv);
    properties.set("max-width"sv, "none"sv);

    JsonObject computed_style;
    computed_style.set("flexGrow"sv, 1);
    computed_style.set("flexShrink"sv, 1);

    JsonObject item;
    item.set("nodeId"sv, node_id.value());
    item.set("flexItemSizing"sv, move(sizing));
    item.set("properties"sv, move(properties));
    item.set("computedStyle"sv, move(computed_style));
    return item;
}

static JsonObject make_flexbox_layout()
{
    JsonArray items;
    items.must_append(make_flex_item(Web::UniqueNodeID { 11 }, 40, 20, "growing"sv, "unclamped"sv));
    items.must_append(make_flex_item(Web::UniqueNodeID { 12 }, 80, -10, "shrinking"sv, "clamped_to_min"sv));

    JsonObject properties;
    properties.set("align-content"sv, "normal"sv);
    properties.set("align-items"sv, "stretch"sv);
    properties.set("flex-direction"sv, "row"sv);
    properties.set("flex-wrap"sv, "nowrap"sv);
    properties.set("justify-content"sv, "flex-start"sv);

    JsonObject layout;
    layout.set("containerNodeId"sv, Web::UniqueNodeID { 10 }.value());
    layout.set("properties"sv, move(properties));
    layout.set("items"sv, move(items));
    return layout;
}

class TestDevToolsDelegate final : public DevTools::DevToolsDelegate {
public:
    virtual Vector<DevTools::TabDescription> tab_list() const override
    {
        return { { .id = 1, .title = "Fixture page"_string, .url = "https://example.test/"_string } };
    }

    virtual Vector<DevTools::CSSProperty> css_property_list() const override
    {
        Vector<DevTools::CSSProperty> properties;
        properties.append({ "display"_string, false });
        properties.append({ "color"_string, true });
        return properties;
    }

    virtual void reload_tab(DevTools::TabDescription const&, bool bypass_cache) const override
    {
        ++reload_tab_call_count;
        last_reload_bypass_cache = bypass_cache;
    }

    virtual void navigate_tab(DevTools::TabDescription const&, String const& url) const override
    {
        ++navigate_tab_call_count;
        last_navigated_url = url;
    }

    virtual void traverse_the_history_by_delta(DevTools::TabDescription const&, int delta) const override
    {
        ++traverse_the_history_by_delta_call_count;
        last_history_delta = delta;
    }

    virtual void inspect_tab(DevTools::TabDescription const&, OnTabInspectionComplete callback) const override
    {
        ++inspect_tab_call_count;
        callback(use_navigation_dom_tree ? make_navigation_dom_tree() : make_dom_tree());
    }

    virtual void inspect_accessibility_tree(DevTools::TabDescription const&, OnAccessibilityTreeInspectionComplete callback) const override
    {
        ++inspect_accessibility_tree_call_count;
        callback(make_accessibility_tree());
    }

    virtual void listen_for_dom_properties(DevTools::TabDescription const&, OnDOMNodePropertiesReceived callback) const override
    {
        ++listen_for_dom_properties_call_count;
        on_dom_node_properties = move(callback);
    }

    virtual void stop_listening_for_dom_properties(DevTools::TabDescription const&) const override
    {
        ++stop_listening_for_dom_properties_call_count;
    }

    virtual void inspect_dom_node(DevTools::TabDescription const&, WebView::DOMNodeProperties::Type type, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element, JsonObject options = {}) const override
    {
        ++inspect_dom_node_call_count;
        last_inspected_dom_node = node_id;
        last_inspected_pseudo_element = pseudo_element;
        last_inspected_dom_node_options = move(options);

        Core::deferred_invoke([this, type] {
            VERIFY(on_dom_node_properties);
            if (type == WebView::DOMNodeProperties::Type::AppliedStyleRules)
                on_dom_node_properties(make_applied_style_rules());
            else if (type == WebView::DOMNodeProperties::Type::ComputedStyle)
                on_dom_node_properties(make_computed_style());
            else if (type == WebView::DOMNodeProperties::Type::Layout)
                on_dom_node_properties(make_layout());
            else
                on_dom_node_properties(make_used_fonts());
        });
    }

    virtual void clear_inspected_dom_node(DevTools::TabDescription const&) const override { ++clear_inspected_dom_node_call_count; }
    virtual void clear_highlighted_dom_node(DevTools::TabDescription const&) const override { ++clear_highlighted_dom_node_call_count; }

    virtual void start_node_picker(DevTools::TabDescription const&, OnNodePickerEvent callback) const override
    {
        ++start_node_picker_call_count;
        on_node_picker_event = move(callback);
    }

    virtual void stop_node_picker(DevTools::TabDescription const&) const override
    {
        ++stop_node_picker_call_count;
    }

    virtual void clear_node_picker(DevTools::TabDescription const&) const override
    {
        ++clear_node_picker_call_count;
    }

    virtual void inspect_grid_layouts(DevTools::TabDescription const&, Web::UniqueNodeID root_node_id, OnGridLayoutsReceived callback) const override
    {
        ++inspect_grid_layouts_call_count;
        last_grid_root_node = root_node_id;
        callback(make_grid_layouts());
    }

    virtual void inspect_current_grid(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnCurrentGridReceived callback) const override
    {
        ++inspect_current_grid_call_count;
        last_current_grid_node = node_id;

        if (node_id == Web::UniqueNodeID { 4 }) {
            callback(make_grid_layout(node_id, "content"sv));
            return;
        }

        callback({});
    }

    virtual void inspect_current_flexbox(DevTools::TabDescription const&, Web::UniqueNodeID node_id, bool only_look_at_parents, OnCurrentFlexboxReceived callback) const override
    {
        ++inspect_current_flexbox_call_count;
        last_current_flexbox_node = node_id;
        last_current_flexbox_only_look_at_parents = only_look_at_parents;

        if ((node_id == Web::UniqueNodeID { 10 } && !only_look_at_parents)
            || node_id == Web::UniqueNodeID { 11 }
            || node_id == Web::UniqueNodeID { 12 }) {
            callback(make_flexbox_layout());
            return;
        }

        callback({});
    }

    virtual void highlight_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) const override
    {
        ++highlight_dom_node_call_count;
        last_highlighted_dom_node = node_id;
        last_highlighted_pseudo_element = pseudo_element;
    }

    virtual void highlight_flexbox(DevTools::TabDescription const&, Web::UniqueNodeID node_id, JsonValue options) const override
    {
        ++highlight_flexbox_call_count;
        last_highlighted_flexbox_node = node_id;
        last_flexbox_highlight_options = move(options);
    }

    virtual void clear_flexbox_highlight(DevTools::TabDescription const&, Web::UniqueNodeID node_id) const override
    {
        ++clear_flexbox_highlight_call_count;
        last_cleared_flexbox_node = node_id;
    }

    virtual void highlight_grid(DevTools::TabDescription const&, Web::UniqueNodeID node_id, JsonValue options) const override
    {
        ++highlight_grid_call_count;
        last_highlighted_grid_node = node_id;
        last_grid_highlight_options = move(options);
    }

    virtual void clear_grid_highlight(DevTools::TabDescription const&, Web::UniqueNodeID node_id) const override
    {
        ++clear_grid_highlight_call_count;
        last_cleared_grid_node = node_id;
    }

    virtual void listen_for_dom_mutations(DevTools::TabDescription const&, OnDOMMutationReceived callback) const override
    {
        ++listen_for_dom_mutations_call_count;
        on_dom_mutation = move(callback);
    }

    virtual void stop_listening_for_dom_mutations(DevTools::TabDescription const&) const override { ++stop_listening_for_dom_mutations_call_count; }

    virtual void get_dom_node_inner_html(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeHTMLReceived callback) const override
    {
        ++get_dom_node_inner_html_call_count;
        last_edited_node = node_id;
        callback("<span>inner</span>"_string);
    }

    virtual void get_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeHTMLReceived callback) const override
    {
        ++get_dom_node_outer_html_call_count;
        last_edited_node = node_id;
        callback("<div id=\"target\"></div>"_string);
    }

    virtual void set_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& html, OnDOMNodeEditComplete callback) const override
    {
        ++set_dom_node_outer_html_call_count;
        last_edited_node = node_id;
        last_html = html;
        callback(node_id);
    }

    virtual void set_dom_node_text(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& text, OnDOMNodeEditComplete callback) const override
    {
        ++set_dom_node_text_call_count;
        last_edited_node = node_id;
        last_text = text;
        callback(node_id);
    }

    virtual void set_dom_node_tag(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& tag_name, OnDOMNodeEditComplete callback) const override
    {
        ++set_dom_node_tag_call_count;
        last_edited_node = node_id;
        last_tag = tag_name;
        callback(node_id);
    }

    virtual void add_dom_node_attributes(DevTools::TabDescription const&, Web::UniqueNodeID node_id, ReadonlySpan<WebView::Attribute> attributes, OnDOMNodeEditComplete callback) const override
    {
        ++add_dom_node_attributes_call_count;
        last_edited_node = node_id;
        last_attribute_count = attributes.size();
        callback(node_id);
    }

    virtual void replace_dom_node_attribute(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& attribute, ReadonlySpan<WebView::Attribute> attributes, OnDOMNodeEditComplete callback) const override
    {
        ++replace_dom_node_attribute_call_count;
        last_edited_node = node_id;
        last_attribute = attribute;
        last_attribute_count = attributes.size();
        callback(node_id);
    }

    virtual void create_child_element(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeEditComplete callback) const override
    {
        ++create_child_element_call_count;
        last_edited_node = node_id;
        callback(node_id);
    }

    virtual void insert_dom_node_before(DevTools::TabDescription const&, Web::UniqueNodeID node_id, Web::UniqueNodeID parent_id, Optional<Web::UniqueNodeID> sibling_id, OnDOMNodeEditComplete callback) const override
    {
        ++insert_dom_node_before_call_count;
        last_edited_node = node_id;
        last_parent_node = parent_id;
        last_sibling_node = sibling_id;
        callback(node_id);
    }

    virtual void clone_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeEditComplete callback) const override
    {
        ++clone_dom_node_call_count;
        last_edited_node = node_id;
        callback(node_id);
    }

    virtual void remove_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeEditComplete callback) const override
    {
        ++remove_dom_node_call_count;
        last_edited_node = node_id;
        callback(node_id);
    }

    virtual void retrieve_style_sheets(DevTools::TabDescription const&, OnStyleSheetsReceived callback) const override
    {
        ++retrieve_style_sheets_call_count;
        Vector<Web::CSS::StyleSheetIdentifier> style_sheets;
        style_sheets.append(fixture_style_sheet());
        style_sheets.append(fixture_user_agent_style_sheet());
        callback(move(style_sheets));
    }

    virtual void retrieve_style_sheet_source(DevTools::TabDescription const&, Web::CSS::StyleSheetIdentifier const&) const override
    {
        ++retrieve_style_sheet_source_call_count;
        Core::deferred_invoke([this] {
            VERIFY(on_style_sheet_source);
            on_style_sheet_source(fixture_style_sheet(), "body { color: red; }"_string);
        });
    }

    virtual void listen_for_style_sheet_sources(DevTools::TabDescription const&, OnStyleSheetSourceReceived callback) const override
    {
        ++listen_for_style_sheet_sources_call_count;
        on_style_sheet_source = move(callback);
    }

    virtual void stop_listening_for_style_sheet_sources(DevTools::TabDescription const&) const override { ++stop_listening_for_style_sheet_sources_call_count; }

    virtual void listen_for_console_messages(DevTools::TabDescription const&, OnConsoleMessage callback) const override
    {
        ++listen_for_console_messages_call_count;
        on_console_message = move(callback);
    }

    virtual void stop_listening_for_console_messages(DevTools::TabDescription const&) const override { ++stop_listening_for_console_messages_call_count; }

    virtual void listen_for_network_events(DevTools::TabDescription const&, OnNetworkRequestStarted on_request_started, OnNetworkResponseHeadersReceived on_headers_received, OnNetworkResponseBodyReceived on_body_received, OnNetworkRequestFinished on_request_finished) const override
    {
        ++listen_for_network_events_call_count;
        on_network_request_started = move(on_request_started);
        on_network_response_headers_received = move(on_headers_received);
        on_network_response_body_received = move(on_body_received);
        on_network_request_finished = move(on_request_finished);
    }

    virtual void stop_listening_for_network_events(DevTools::TabDescription const&) const override { ++stop_listening_for_network_events_call_count; }

    virtual void listen_for_navigation_events(DevTools::TabDescription const&, OnNavigationStarted on_started, OnNavigationFinished on_finished) const override
    {
        ++listen_for_navigation_events_call_count;
        navigation_listeners.append({ move(on_started), move(on_finished) });
    }

    virtual void stop_listening_for_navigation_events(DevTools::TabDescription const&) const override
    {
        ++stop_listening_for_navigation_events_call_count;
        if (!navigation_listeners.is_empty())
            navigation_listeners.take_first();
    }
    virtual void did_connect_devtools_client(DevTools::TabDescription const&) const override { ++did_connect_devtools_client_call_count; }
    virtual void did_disconnect_devtools_client(DevTools::TabDescription const&) const override { ++did_disconnect_devtools_client_call_count; }

    void emit_mutation(WebView::Mutation mutation) const
    {
        VERIFY(on_dom_mutation);
        on_dom_mutation(move(mutation));
    }

    void emit_console_log(JS::Console::LogLevel level, Vector<JsonValue> arguments) const
    {
        VERIFY(on_console_message);
        on_console_message({ UnixDateTime::from_seconds_since_epoch(10), WebView::ConsoleLog { level, move(arguments) } });
    }

    void emit_console_trace() const
    {
        VERIFY(on_console_message);
        Vector<WebView::StackFrame> stack;
        stack.append({ "traceFunction"_string, "trace.js"_string, 12, 34 });
        on_console_message({ UnixDateTime::from_seconds_since_epoch(11), WebView::ConsoleTrace { "trace label"_string, move(stack) } });
    }

    void emit_console_error() const
    {
        VERIFY(on_console_message);
        Vector<WebView::StackFrame> stack;
        stack.append({ "boom"_string, "error.js"_string, 56, 78 });
        on_console_message({ UnixDateTime::from_seconds_since_epoch(12), WebView::ConsoleError { "TypeError"_string, "bad things"_string, move(stack), true } });
    }

    void emit_network_lifecycle() const
    {
        VERIFY(on_network_request_started);
        VERIFY(on_network_response_headers_received);
        VERIFY(on_network_response_body_received);
        VERIFY(on_network_request_finished);

        ByteBuffer request_body;
        request_body.append("request", 7);

        Vector<HTTP::Header> request_headers;
        request_headers.append({ ByteString::formatted("Accept"), ByteString::formatted("*/*") });

        on_network_request_started({ .request_id = 100,
            .url = "https://example.test/data.json"_string,
            .method = "POST"_string,
            .start_time = UnixDateTime::from_seconds_since_epoch(20),
            .request_headers = move(request_headers),
            .request_body = move(request_body),
            .initiator_type = "fetch"_string });

        Vector<HTTP::Header> response_headers;
        response_headers.append({ ByteString::formatted("Content-Type"), ByteString::formatted("application/json") });
        on_network_response_headers_received({ .request_id = 100,
            .status_code = 200,
            .reason_phrase = "OK"_string,
            .response_headers = move(response_headers) });

        ByteBuffer response_body;
        response_body.append("{\"ok\":true}", 11);
        on_network_response_body_received(100, move(response_body));

        Requests::RequestTimingInfo timing_info;
        timing_info.request_start_microseconds = 1000;
        timing_info.domain_lookup_start_microseconds = 1000;
        timing_info.domain_lookup_end_microseconds = 2000;
        timing_info.connect_start_microseconds = 2000;
        timing_info.connect_end_microseconds = 4000;
        timing_info.response_start_microseconds = 5000;
        timing_info.response_end_microseconds = 9000;
        on_network_request_finished({ .request_id = 100, .body_size = 11, .timing_info = timing_info, .network_error = {} });
    }

    void emit_navigation() const
    {
        emit_navigation_start();
        emit_navigation_finish();
    }

    void emit_navigation_start() const
    {
        VERIFY(!navigation_listeners.is_empty());
        auto listener_count = navigation_listeners.size();
        for (size_t i = 0; i < listener_count; ++i)
            navigation_listeners[i].on_navigation_started("https://example.test/next"_string);
    }

    void emit_navigation_finish() const
    {
        VERIFY(!navigation_listeners.is_empty());
        auto listener_count = navigation_listeners.size();
        for (size_t i = 0; i < listener_count; ++i)
            navigation_listeners[i].on_navigation_finished("https://example.test/next"_string, "Next page"_string);
    }

    size_t navigation_listener_count() const
    {
        return navigation_listeners.size();
    }

    void switch_to_navigation_dom_tree() const
    {
        use_navigation_dom_tree = true;
    }

    void emit_node_picker_event(DevToolsDelegate::NodePickerEvent event) const
    {
        VERIFY(on_node_picker_event);
        on_node_picker_event(move(event));
    }

    mutable Function<void(WebView::DOMNodeProperties)> on_dom_node_properties;
    mutable Function<void(WebView::Mutation)> on_dom_mutation;
    mutable Function<void(Web::CSS::StyleSheetIdentifier const&, String)> on_style_sheet_source;
    mutable Function<void(WebView::ConsoleOutput)> on_console_message;
    mutable Function<void(DevToolsDelegate::NetworkRequestData)> on_network_request_started;
    mutable Function<void(DevToolsDelegate::NetworkResponseData)> on_network_response_headers_received;
    mutable Function<void(u64, ByteBuffer)> on_network_response_body_received;
    mutable Function<void(DevToolsDelegate::NetworkRequestCompleteData)> on_network_request_finished;
    mutable Function<void(DevToolsDelegate::NodePickerEvent)> on_node_picker_event;

    struct NavigationListener {
        Function<void(String)> on_navigation_started;
        Function<void(String, String)> on_navigation_finished;
    };
    mutable Vector<NavigationListener> navigation_listeners;

    mutable bool use_navigation_dom_tree { false };

    mutable size_t inspect_tab_call_count { 0 };
    mutable size_t inspect_accessibility_tree_call_count { 0 };
    mutable size_t listen_for_dom_properties_call_count { 0 };
    mutable size_t stop_listening_for_dom_properties_call_count { 0 };
    mutable size_t inspect_dom_node_call_count { 0 };
    mutable size_t clear_inspected_dom_node_call_count { 0 };
    mutable size_t start_node_picker_call_count { 0 };
    mutable size_t stop_node_picker_call_count { 0 };
    mutable size_t clear_node_picker_call_count { 0 };
    mutable size_t inspect_grid_layouts_call_count { 0 };
    mutable size_t inspect_current_grid_call_count { 0 };
    mutable size_t inspect_current_flexbox_call_count { 0 };
    mutable size_t highlight_dom_node_call_count { 0 };
    mutable size_t clear_highlighted_dom_node_call_count { 0 };
    mutable size_t highlight_flexbox_call_count { 0 };
    mutable size_t clear_flexbox_highlight_call_count { 0 };
    mutable size_t highlight_grid_call_count { 0 };
    mutable size_t clear_grid_highlight_call_count { 0 };
    mutable size_t listen_for_dom_mutations_call_count { 0 };
    mutable size_t stop_listening_for_dom_mutations_call_count { 0 };
    mutable size_t get_dom_node_inner_html_call_count { 0 };
    mutable size_t get_dom_node_outer_html_call_count { 0 };
    mutable size_t set_dom_node_outer_html_call_count { 0 };
    mutable size_t set_dom_node_text_call_count { 0 };
    mutable size_t set_dom_node_tag_call_count { 0 };
    mutable size_t add_dom_node_attributes_call_count { 0 };
    mutable size_t replace_dom_node_attribute_call_count { 0 };
    mutable size_t create_child_element_call_count { 0 };
    mutable size_t insert_dom_node_before_call_count { 0 };
    mutable size_t clone_dom_node_call_count { 0 };
    mutable size_t remove_dom_node_call_count { 0 };
    mutable size_t retrieve_style_sheets_call_count { 0 };
    mutable size_t retrieve_style_sheet_source_call_count { 0 };
    mutable size_t listen_for_style_sheet_sources_call_count { 0 };
    mutable size_t stop_listening_for_style_sheet_sources_call_count { 0 };
    mutable size_t listen_for_console_messages_call_count { 0 };
    mutable size_t stop_listening_for_console_messages_call_count { 0 };
    mutable size_t listen_for_network_events_call_count { 0 };
    mutable size_t stop_listening_for_network_events_call_count { 0 };
    mutable size_t listen_for_navigation_events_call_count { 0 };
    mutable size_t stop_listening_for_navigation_events_call_count { 0 };
    mutable size_t did_connect_devtools_client_call_count { 0 };
    mutable size_t did_disconnect_devtools_client_call_count { 0 };
    mutable size_t navigate_tab_call_count { 0 };
    mutable size_t reload_tab_call_count { 0 };
    mutable size_t traverse_the_history_by_delta_call_count { 0 };

    mutable Optional<Web::UniqueNodeID> last_highlighted_dom_node;
    mutable Optional<Web::CSS::PseudoElement> last_highlighted_pseudo_element;
    mutable Optional<Web::UniqueNodeID> last_inspected_dom_node;
    mutable Optional<Web::CSS::PseudoElement> last_inspected_pseudo_element;
    mutable JsonObject last_inspected_dom_node_options;
    mutable Optional<Web::UniqueNodeID> last_grid_root_node;
    mutable Optional<Web::UniqueNodeID> last_current_grid_node;
    mutable Optional<Web::UniqueNodeID> last_current_flexbox_node;
    mutable bool last_current_flexbox_only_look_at_parents { false };
    mutable Optional<Web::UniqueNodeID> last_highlighted_flexbox_node;
    mutable Optional<Web::UniqueNodeID> last_cleared_flexbox_node;
    mutable JsonValue last_flexbox_highlight_options;
    mutable Optional<Web::UniqueNodeID> last_highlighted_grid_node;
    mutable Optional<Web::UniqueNodeID> last_cleared_grid_node;
    mutable JsonValue last_grid_highlight_options;
    mutable Optional<Web::UniqueNodeID> last_edited_node;
    mutable Optional<Web::UniqueNodeID> last_parent_node;
    mutable Optional<Web::UniqueNodeID> last_sibling_node;
    mutable Optional<String> last_html;
    mutable Optional<String> last_text;
    mutable Optional<String> last_tag;
    mutable Optional<String> last_attribute;
    mutable size_t last_attribute_count { 0 };
    mutable Optional<String> last_navigated_url;
    mutable Optional<bool> last_reload_bypass_cache;
    mutable Optional<int> last_history_delta;
};

class ProtocolClient {
public:
    static NonnullOwnPtr<ProtocolClient> connect(Core::EventLoop& loop, DevTools::DevToolsServer& server)
    {
        auto port = server.local_port();
        VERIFY(port.has_value());
        auto socket = MUST(Core::TCPSocket::connect("127.0.0.1", *port));
        MUST(socket->set_blocking(false));

        auto client = adopt_own(*new ProtocolClient(loop, move(socket)));
        spin_until(loop, [&] { return !!server.connection(); });
        return client;
    }

    JsonObject read_message()
    {
        if (!m_pending_messages.is_empty()) {
            auto message = move(m_pending_messages.first());
            m_pending_messages.remove(0);
            return message;
        }
        return read_message_from_socket();
    }

    void send(JsonObject message)
    {
        auto serialized = message.serialized();
        auto packet = MUST(String::formatted("{}:{}", serialized.byte_count(), serialized));
        MUST(m_socket->set_blocking(true));
        auto restore_nonblocking = ScopeGuard([&] { MUST(m_socket->set_blocking(false)); });
        MUST(m_socket->write_until_depleted(packet.bytes()));
    }

    JsonObject request(StringView to, StringView type)
    {
        JsonObject message;
        message.set("to"sv, to);
        message.set("type"sv, type);
        return request(move(message), to);
    }

    JsonObject request(JsonObject message)
    {
        auto to = message.get_string("to"sv).value_or("root"_string);
        return request(move(message), to);
    }

private:
    ProtocolClient(Core::EventLoop& loop, NonnullOwnPtr<Core::TCPSocket> socket)
        : m_loop(loop)
        , m_socket(move(socket))
    {
    }

    static bool is_event_packet(JsonObject const& packet)
    {
        auto type = packet.get_string("type"sv);
        if (!type.has_value())
            return false;
        return *type == "frameUpdate"sv
            || *type == "resources-available-array"sv
            || *type == "resources-updated-array"sv
            || *type == "newMutations"sv
            || *type == "pickerNodeCanceled"sv
            || *type == "pickerNodeHovered"sv
            || *type == "pickerNodePicked"sv
            || *type == "pickerNodePreviewed"sv
            || *type == "tabListChanged"sv
            || *type == "target-available-form"sv
            || *type == "target-destroyed-form"sv;
    }

    JsonObject request(JsonObject message, StringView response_actor)
    {
        send(move(message));
        while (true) {
            auto response = read_message_from_socket();
            if (response.get_string("from"sv).value_or({}) == response_actor && !is_event_packet(response))
                return response;
            m_pending_messages.append(move(response));
        }
    }

    JsonObject read_message_from_socket()
    {
        Optional<JsonObject> message;
        spin_until(m_loop, [&] {
            pump(m_loop);
            if (!MUST(m_socket->can_read_without_blocking()))
                return false;
            message = read_message_now();
            return true;
        });
        return message.release_value();
    }

    JsonObject read_message_now()
    {
        MUST(m_socket->set_blocking(true));
        auto restore_nonblocking = ScopeGuard([&] { MUST(m_socket->set_blocking(false)); });

        ByteBuffer length_buffer;
        while (true) {
            auto byte = MUST(m_socket->read_value<u8>());
            if (byte == ':')
                break;
            length_buffer.append(byte);
        }

        auto length = StringView { length_buffer }.to_number<size_t>();
        VERIFY(length.has_value());

        ByteBuffer message_buffer;
        message_buffer.resize(*length);
        MUST(m_socket->read_until_filled(message_buffer));

        auto value = JsonValue::from_string(message_buffer);
        VERIFY(!value.is_error());
        VERIFY(value.value().is_object());
        return value.release_value().as_object();
    }

    Core::EventLoop& m_loop;
    NonnullOwnPtr<Core::TCPSocket> m_socket;
    Vector<JsonObject> m_pending_messages;
};

struct TestSession {
    Core::EventLoop loop;
    TestDevToolsDelegate delegate;
    OwnPtr<DevTools::DevToolsServer> server;
    OwnPtr<ProtocolClient> client;
};

static NonnullOwnPtr<TestSession> create_session()
{
    auto session = make<TestSession>();
    session->server = MUST(DevTools::DevToolsServer::create(session->delegate, 0));
    session->client = ProtocolClient::connect(session->loop, *session->server);
    return session;
}

static String actor_from(JsonObject const& object, StringView key)
{
    return object.get_string(key).release_value();
}

static JsonObject get_tab(ProtocolClient& client)
{
    auto tabs = client.request("root"sv, "listTabs"sv).get_array("tabs"sv).release_value();
    VERIFY(tabs.size() == 1u);
    return tabs.at(0).as_object();
}

static size_t style_rule_actor_count(DevTools::DevToolsServer const& server)
{
    size_t count = 0;
    for (auto const& actor : server.actor_registry()) {
        if (actor.key.bytes_as_string_view().contains("-style-rule"sv))
            ++count;
    }
    return count;
}

static JsonObject get_frame_target(ProtocolClient& client, StringView tab_actor)
{
    auto watcher_actor = actor_from(client.request(tab_actor, "getWatcher"sv), "actor"sv);

    JsonObject request;
    request.set("to"sv, watcher_actor);
    request.set("type"sv, "watchTargets"sv);
    request.set("targetType"sv, "frame"sv);

    EXPECT_EQ(client.request(move(request)).get_string("from"sv).value(), watcher_actor);

    while (true) {
        auto message = client.read_message();
        if (message.get_string("type"sv).value_or({}) == "target-available-form"sv)
            return message.get_object("target"sv).release_value();
    }
}

static JsonObject get_walker(ProtocolClient& client, StringView inspector_actor)
{
    return client.request(inspector_actor, "getWalker"sv).get_object("walker"sv).release_value();
}

static String query_selector(ProtocolClient& client, StringView walker_actor, StringView root_node, StringView selector)
{
    JsonObject request;
    request.set("to"sv, walker_actor);
    request.set("type"sv, "querySelector"sv);
    request.set("node"sv, root_node);
    request.set("selector"sv, selector);
    return client.request(move(request)).get_object("node"sv)->get_string("actor"sv).release_value();
}

static JsonObject read_resource(ProtocolClient& client, StringView resource_type, StringView packet_type = "resources-available-array"sv)
{
    while (true) {
        auto message = client.read_message();
        if (message.get_string("type"sv).value_or({}) != packet_type)
            continue;

        auto array = message.get_array("array"sv);
        if (!array.has_value())
            continue;

        for (auto const& entry : array->values()) {
            if (!entry.is_array() || entry.as_array().size() < 2)
                continue;
            if (!entry.as_array().at(0).is_string() || entry.as_array().at(0).as_string() != resource_type)
                continue;
            auto const& resources = entry.as_array().at(1).as_array();
            VERIFY(!resources.is_empty());
            return resources.at(0).as_object();
        }
    }
}

static JsonObject read_packet_with_type(ProtocolClient& client, StringView packet_type)
{
    while (true) {
        auto message = client.read_message();
        if (message.get_string("type"sv).value_or({}) == packet_type)
            return message;
    }
}

TEST_CASE(root_actor_and_connection_errors)
{
    auto session = create_session();
    auto& client = *session->client;

    auto greeting = client.read_message();
    EXPECT_EQ(greeting.get_string("applicationType"sv).value(), "browser"sv);
    auto traits = greeting.get_object("traits"sv).release_value();
    EXPECT(traits.get_bool("highlightable"sv).value());
    EXPECT(traits.get_bool("customHighlighters"sv).value());
    EXPECT(traits.get_bool("networkMonitor"sv).value());

    EXPECT_EQ(client.request("root"sv, "connect"sv).get_string("from"sv).value(), "root"sv);
    auto root = client.request("root"sv, "getRoot"sv);
    EXPECT(root.has_string("deviceActor"sv));
    EXPECT(root.has_string("parentAccessibilityActor"sv));
    EXPECT(root.has_string("preferenceActor"sv));

    EXPECT_EQ(client.request("root"sv, "listProcesses"sv).get_array("processes"sv)->size(), 1u);
    EXPECT(client.request("root"sv, "listAddons"sv).get_array("addons"sv)->is_empty());
    EXPECT(client.request("root"sv, "listWorkers"sv).get_array("workers"sv)->is_empty());
    EXPECT(client.request("root"sv, "listServiceWorkerRegistrations"sv).get_array("registrations"sv)->is_empty());

    auto tab = get_tab(client);
    auto tab_actor = actor_from(tab, "actor"sv);
    EXPECT_EQ(tab.get_string("title"sv).value(), "Fixture page"sv);
    EXPECT_EQ(tab.get_integer<u64>("browserId"sv).value(), 1u);
    auto tab_traits = tab.get_object("traits"sv).release_value();
    EXPECT(tab_traits.get_bool("supportsReloadDescriptor"sv).value());
    EXPECT(tab_traits.get_bool("supportsNavigation"sv).value());

    JsonObject get_tab_request;
    get_tab_request.set("to"sv, "root"sv);
    get_tab_request.set("type"sv, "getTab"sv);
    get_tab_request.set("browserId"sv, 1);
    EXPECT_EQ(client.request(move(get_tab_request)).get_object("tab"sv)->get_string("actor"sv).value(), tab_actor);

    session->server->refresh_tab_list();
    EXPECT_EQ(client.read_message().get_string("type"sv).value(), "tabListChanged"sv);
    session->server->refresh_tab_list();
    pump(session->loop);

    JsonObject missing_to;
    missing_to.set("type"sv, "connect"sv);
    client.send(move(missing_to));
    EXPECT_EQ(client.read_message().get_string("error"sv).value(), "missingParameter"sv);

    JsonObject missing_type;
    missing_type.set("to"sv, "root"sv);
    client.send(move(missing_type));
    EXPECT_EQ(client.read_message().get_string("error"sv).value(), "missingParameter"sv);

    JsonObject unknown_actor;
    unknown_actor.set("to"sv, "missing"sv);
    unknown_actor.set("type"sv, "connect"sv);
    client.send(move(unknown_actor));
    EXPECT_EQ(client.read_message().get_string("error"sv).value(), "unknownActor"sv);
    EXPECT_EQ(client.request("root"sv, "notARealPacket"sv).get_string("error"sv).value(), "unrecognizedPacketType"sv);

    JsonObject first;
    first.set("to"sv, "root"sv);
    first.set("type"sv, "listWorkers"sv);
    JsonObject second;
    second.set("to"sv, "root"sv);
    second.set("type"sv, "listAddons"sv);
    client.send(move(first));
    client.send(move(second));
    EXPECT(client.read_message().has_array("workers"sv));
    EXPECT(client.read_message().has_array("addons"sv));
}

TEST_CASE(history_navigation_requests)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto tab_actor = actor_from(get_tab(client), "actor"sv);

    JsonObject navigate_to;
    navigate_to.set("to"sv, tab_actor);
    navigate_to.set("type"sv, "navigateTo"sv);
    navigate_to.set("url"sv, "https://example.test/from-devtools"sv);
    navigate_to.set("waitForLoad"sv, false);
    EXPECT_EQ(client.request(move(navigate_to)).get_string("from"sv).value(), tab_actor);
    EXPECT_EQ(session->delegate.navigate_tab_call_count, 1u);
    EXPECT_EQ(session->delegate.last_navigated_url.value(), "https://example.test/from-devtools"sv);

    EXPECT_EQ(client.request(tab_actor, "goBack"sv).get_string("from"sv).value(), tab_actor);
    EXPECT_EQ(session->delegate.traverse_the_history_by_delta_call_count, 1u);
    EXPECT_EQ(session->delegate.last_history_delta.value(), -1);

    EXPECT_EQ(client.request(tab_actor, "goForward"sv).get_string("from"sv).value(), tab_actor);
    EXPECT_EQ(session->delegate.traverse_the_history_by_delta_call_count, 2u);
    EXPECT_EQ(session->delegate.last_history_delta.value(), 1);

    auto target = get_frame_target(client, tab_actor);
    auto target_actor = actor_from(target, "actor"sv);

    EXPECT_EQ(client.request(target_actor, "goBack"sv).get_string("from"sv).value(), target_actor);
    EXPECT_EQ(session->delegate.traverse_the_history_by_delta_call_count, 3u);
    EXPECT_EQ(session->delegate.last_history_delta.value(), -1);

    EXPECT_EQ(client.request(target_actor, "goForward"sv).get_string("from"sv).value(), target_actor);
    EXPECT_EQ(session->delegate.traverse_the_history_by_delta_call_count, 4u);
    EXPECT_EQ(session->delegate.last_history_delta.value(), 1);
}

TEST_CASE(target_bootstrap_and_lifetime)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto tab_actor = actor_from(get_tab(client), "actor"sv);
    EXPECT(client.request(tab_actor, "getFavicon"sv).get("favicon"sv).value().is_null());

    auto watcher_actor = actor_from(client.request(tab_actor, "getWatcher"sv), "actor"sv);
    EXPECT_EQ(actor_from(client.request(tab_actor, "getWatcher"sv), "actor"sv), watcher_actor);
    EXPECT(client.request(watcher_actor, "getTargetConfigurationActor"sv).has_object("configuration"sv));
    EXPECT(client.request(watcher_actor, "getThreadConfigurationActor"sv).has_object("configuration"sv));
    EXPECT(client.request(watcher_actor, "getNetworkParentActor"sv).has_string("network"sv));

    JsonObject watch_resources;
    watch_resources.set("to"sv, watcher_actor);
    watch_resources.set("type"sv, "watchResources"sv);
    JsonArray resource_types;
    resource_types.must_append("console-message"sv);
    watch_resources.set("resourceTypes"sv, move(resource_types));
    EXPECT_EQ(client.request(move(watch_resources)).get_string("from"sv).value(), watcher_actor);

    auto target = get_frame_target(client, tab_actor);
    EXPECT_EQ(target.get_string("targetType"sv).value(), "frame"sv);
    EXPECT(target.has_string("consoleActor"sv));
    EXPECT(target.has_string("inspectorActor"sv));
    EXPECT(target.has_string("styleSheetsActor"sv));
    EXPECT(target.has_string("threadActor"sv));
    EXPECT(target.has_string("accessibilityActor"sv));
    EXPECT_EQ(session->delegate.did_connect_devtools_client_call_count, 1u);
    EXPECT_EQ(session->delegate.listen_for_console_messages_call_count, 1u);
    EXPECT_EQ(session->delegate.listen_for_network_events_call_count, 1u);
    EXPECT_EQ(session->delegate.retrieve_style_sheets_call_count, 1u);

    EXPECT_EQ(client.request(actor_from(target, "actor"sv), "detach"sv).get_string("from"sv).value(), actor_from(target, "actor"sv));
    EXPECT_EQ(session->delegate.stop_listening_for_console_messages_call_count, 1u);
    EXPECT_EQ(session->delegate.stop_listening_for_dom_mutations_call_count, 1u);
    EXPECT_EQ(session->delegate.clear_highlighted_dom_node_call_count, 1u);
    EXPECT_EQ(session->delegate.clear_inspected_dom_node_call_count, 1u);
}

TEST_CASE(walker_node_picker)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto inspector_actor = actor_from(target, "inspectorActor"sv);
    auto walker = get_walker(client, inspector_actor);
    auto walker_actor = actor_from(walker, "actor"sv);
    auto root_node_actor = walker.get_object("root"sv)->get_string("actor"sv).release_value();

    EXPECT_EQ(client.request(walker_actor, "pick"sv).get_string("from"sv).value(), walker_actor);
    EXPECT_EQ(session->delegate.start_node_picker_call_count, 1u);

    session->delegate.emit_node_picker_event({
        .type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Hovered,
        .node_id = Web::UniqueNodeID { 5 },
    });

    auto hover_event = read_packet_with_type(client, "pickerNodeHovered"sv);
    EXPECT_EQ(hover_event.get_string("type"sv).value(), "pickerNodeHovered"sv);
    EXPECT_EQ(hover_event.get_object("node"sv)->get_object("node"sv)->get_string("nodeName"sv).value(), "DIV"sv);
    auto const& hover_new_parents = *hover_event.get_object("node"sv)->get_array("newParents"sv);
    EXPECT_EQ(hover_new_parents.size(), 2u);
    EXPECT_EQ(hover_new_parents.at(0).as_object().get_string("nodeName"sv).value(), "BODY"sv);
    EXPECT_EQ(hover_new_parents.at(1).as_object().get_string("nodeName"sv).value(), "HTML"sv);
    EXPECT_EQ(session->delegate.highlight_dom_node_call_count, 1u);
    EXPECT_EQ(session->delegate.last_highlighted_dom_node.value(), Web::UniqueNodeID { 4 });

    session->delegate.emit_node_picker_event({
        .type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Hovered,
        .node_id = Web::UniqueNodeID { 4 },
    });
    EXPECT_EQ(session->delegate.highlight_dom_node_call_count, 1u);

    session->delegate.emit_node_picker_event({
        .type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Previewed,
        .node_id = Web::UniqueNodeID { 8 },
    });

    auto preview_event = read_packet_with_type(client, "pickerNodePreviewed"sv);
    EXPECT_EQ(preview_event.get_string("type"sv).value(), "pickerNodePreviewed"sv);
    EXPECT_EQ(preview_event.get_object("node"sv)->get_object("node"sv)->get_string("nodeName"sv).value(), "SPAN"sv);
    EXPECT_EQ(preview_event.get_object("node"sv)->get_array("newParents"sv)->size(), 2u);
    EXPECT_EQ(session->delegate.stop_node_picker_call_count, 0u);

    session->delegate.emit_node_picker_event({
        .type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Picked,
        .node_id = Web::UniqueNodeID { 5 },
    });

    auto picked_event = read_packet_with_type(client, "pickerNodePicked"sv);
    EXPECT_EQ(picked_event.get_string("type"sv).value(), "pickerNodePicked"sv);
    EXPECT_EQ(picked_event.get_object("node"sv)->get_object("node"sv)->get_string("nodeName"sv).value(), "DIV"sv);
    EXPECT_EQ(picked_event.get_object("node"sv)->get_array("newParents"sv)->size(), 2u);
    EXPECT_EQ(session->delegate.stop_node_picker_call_count, 1u);
    EXPECT_EQ(session->delegate.clear_node_picker_call_count, 1u);

    EXPECT_EQ(client.request(walker_actor, "pick"sv).get_string("from"sv).value(), walker_actor);
    EXPECT_EQ(session->delegate.start_node_picker_call_count, 2u);

    session->delegate.emit_node_picker_event({
        .type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Canceled,
        .node_id = {},
    });

    auto canceled_event = read_packet_with_type(client, "pickerNodeCanceled"sv);
    EXPECT_EQ(canceled_event.get_string("type"sv).value(), "pickerNodeCanceled"sv);
    EXPECT_EQ(session->delegate.stop_node_picker_call_count, 2u);
    EXPECT_EQ(session->delegate.clear_node_picker_call_count, 2u);

    EXPECT_EQ(client.request(walker_actor, "pick"sv).get_string("from"sv).value(), walker_actor);
    EXPECT_EQ(client.request(walker_actor, "clearPicker"sv).get_string("from"sv).value(), walker_actor);
    EXPECT_EQ(session->delegate.clear_node_picker_call_count, 3u);
    EXPECT_EQ(client.request(walker_actor, "cancelPick"sv).get_string("from"sv).value(), walker_actor);
    EXPECT_EQ(session->delegate.stop_node_picker_call_count, 3u);
    EXPECT_EQ(session->delegate.clear_node_picker_call_count, 4u);

    JsonObject children;
    children.set("to"sv, walker_actor);
    children.set("type"sv, "children"sv);
    children.set("node"sv, root_node_actor);
    EXPECT_EQ(client.request(move(children)).get_array("nodes"sv)->size(), 1u);
}

TEST_CASE(inspector_walker_navigation_reloads_root)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto tab = get_tab(client);
    auto tab_actor = actor_from(tab, "actor"sv);
    auto target = get_frame_target(client, tab_actor);
    auto inspector_actor = actor_from(target, "inspectorActor"sv);
    auto walker = get_walker(client, inspector_actor);
    auto walker_actor = actor_from(walker, "actor"sv);
    auto root_node = walker.get_object("root"sv).release_value();
    auto root_node_actor = actor_from(root_node, "actor"sv);
    EXPECT_EQ(session->delegate.navigation_listener_count(), 1u);

    auto div_actor = query_selector(client, walker_actor, root_node_actor, "div"sv);

    JsonObject watch_root;
    watch_root.set("to"sv, walker_actor);
    watch_root.set("type"sv, "watchRootNode"sv);
    EXPECT_EQ(client.request(move(watch_root)).get_string("type"sv).value(), "root-available"sv);

    session->delegate.emit_navigation_start();

    auto root_destroyed = read_packet_with_type(client, "root-destroyed"sv);
    auto destroyed_node = root_destroyed.get_object("node"sv).release_value();
    EXPECT_EQ(destroyed_node.get_string("actor"sv).value(), root_node_actor);
    EXPECT(destroyed_node.get_bool("isTopLevelDocument"sv).value());

    JsonObject is_in_dom_tree;
    is_in_dom_tree.set("to"sv, walker_actor);
    is_in_dom_tree.set("type"sv, "isInDOMTree"sv);
    is_in_dom_tree.set("node"sv, div_actor);
    EXPECT(!client.request(move(is_in_dom_tree)).get_bool("attached"sv).value());

    EXPECT_EQ(session->delegate.clear_highlighted_dom_node_call_count, 1u);
    EXPECT_EQ(session->delegate.clear_inspected_dom_node_call_count, 1u);

    auto will_navigate = read_resource(client, "document-event"sv);
    EXPECT_EQ(will_navigate.get_string("name"sv).value(), "will-navigate"sv);
    EXPECT_EQ(will_navigate.get_string("newURI"sv).value(), "https://example.test/next"sv);
    EXPECT(!will_navigate.has_string("url"sv));

    session->delegate.switch_to_navigation_dom_tree();
    session->delegate.emit_navigation_finish();

    auto root_available = read_packet_with_type(client, "root-available"sv);
    auto new_root_node = root_available.get_object("node"sv).release_value();
    auto new_root_node_actor = actor_from(new_root_node, "actor"sv);
    auto main_actor = query_selector(client, walker_actor, new_root_node_actor, "main"sv);

    JsonObject main_is_in_dom_tree;
    main_is_in_dom_tree.set("to"sv, walker_actor);
    main_is_in_dom_tree.set("type"sv, "isInDOMTree"sv);
    main_is_in_dom_tree.set("node"sv, main_actor);
    EXPECT(client.request(move(main_is_in_dom_tree)).get_bool("attached"sv).value());

    EXPECT_EQ(session->delegate.inspect_tab_call_count, 2u);

    auto target_destroyed = read_packet_with_type(client, "target-destroyed-form"sv);
    auto destroyed_target = target_destroyed.get_object("target"sv).release_value();
    EXPECT_EQ(actor_from(destroyed_target, "actor"sv), actor_from(target, "actor"sv));
    auto options = target_destroyed.get_object("options"sv).release_value();
    EXPECT(options.get_bool("isTargetSwitching"sv).value());
    EXPECT(options.get_bool("shouldDestroyTargetFront"sv).value());

    auto new_target_available = read_packet_with_type(client, "target-available-form"sv);
    auto new_target = new_target_available.get_object("target"sv).release_value();
    EXPECT_NE(actor_from(new_target, "actor"sv), actor_from(target, "actor"sv));
    EXPECT_EQ(new_target.get_string("url"sv).value(), "https://example.test/next"sv);
    EXPECT_NE(
        new_target.get_integer<u64>("innerWindowId"sv).value(),
        target.get_integer<u64>("innerWindowId"sv).value());

    EXPECT_EQ(client.request(actor_from(new_target, "actor"sv), "listFrames"sv).get_string("from"sv).value(), actor_from(new_target, "actor"sv));

    auto dom_loading = read_resource(client, "document-event"sv);
    EXPECT_EQ(dom_loading.get_string("name"sv).value(), "dom-loading"sv);
    EXPECT_EQ(dom_loading.get_string("url"sv).value(), "https://example.test/next"sv);
    EXPECT(!dom_loading.has_string("title"sv));

    auto dom_interactive = read_resource(client, "document-event"sv);
    EXPECT_EQ(dom_interactive.get_string("name"sv).value(), "dom-interactive"sv);
    EXPECT_EQ(dom_interactive.get_string("url"sv).value(), "https://example.test/next"sv);
    EXPECT_EQ(dom_interactive.get_string("title"sv).value(), "Next page"sv);

    auto dom_complete = read_resource(client, "document-event"sv);
    EXPECT_EQ(dom_complete.get_string("name"sv).value(), "dom-complete"sv);
    EXPECT(!dom_complete.has_string("url"sv));
    EXPECT(!dom_complete.has_string("title"sv));

    EXPECT_EQ(session->delegate.navigation_listener_count(), 1u);
    EXPECT_EQ(session->delegate.listen_for_navigation_events_call_count, 2u);
    EXPECT_EQ(session->delegate.stop_listening_for_navigation_events_call_count, 1u);
    EXPECT_EQ(session->delegate.did_connect_devtools_client_call_count, 1u);
    EXPECT_EQ(session->delegate.did_disconnect_devtools_client_call_count, 0u);

    auto new_walker = get_walker(client, actor_from(new_target, "inspectorActor"sv));
    auto new_walker_root = new_walker.get_object("root"sv).release_value();
    auto new_walker_root_actor = actor_from(new_walker_root, "actor"sv);
    auto heading_actor = query_selector(client, actor_from(new_walker, "actor"sv), new_walker_root_actor, "h1"sv);
    EXPECT(!heading_actor.is_empty());

    JsonObject reload_descriptor;
    reload_descriptor.set("to"sv, tab_actor);
    reload_descriptor.set("type"sv, "reloadDescriptor"sv);
    reload_descriptor.set("bypassCache"sv, true);
    EXPECT_EQ(client.request(move(reload_descriptor)).get_string("from"sv).value(), tab_actor);
    EXPECT_EQ(session->delegate.reload_tab_call_count, 1u);
    EXPECT(session->delegate.last_reload_bypass_cache.value());

    session->delegate.emit_navigation_start();
    (void)read_packet_with_type(client, "root-destroyed"sv);

    session->delegate.emit_navigation_finish();
    (void)read_packet_with_type(client, "root-available"sv);

    auto refreshed_target_destroyed = read_packet_with_type(client, "target-destroyed-form"sv);
    auto refreshed_destroyed_target = refreshed_target_destroyed.get_object("target"sv).release_value();
    EXPECT_EQ(actor_from(refreshed_destroyed_target, "actor"sv), actor_from(new_target, "actor"sv));

    auto refreshed_target_available = read_packet_with_type(client, "target-available-form"sv);
    auto refreshed_target = refreshed_target_available.get_object("target"sv).release_value();
    EXPECT_NE(actor_from(refreshed_target, "actor"sv), actor_from(new_target, "actor"sv));
    EXPECT_NE(
        refreshed_target.get_integer<u64>("innerWindowId"sv).value(),
        new_target.get_integer<u64>("innerWindowId"sv).value());
    EXPECT_EQ(session->delegate.navigation_listener_count(), 1u);
    EXPECT_EQ(session->delegate.listen_for_navigation_events_call_count, 3u);
    EXPECT_EQ(session->delegate.stop_listening_for_navigation_events_call_count, 2u);
    EXPECT_EQ(session->delegate.did_connect_devtools_client_call_count, 1u);
    EXPECT_EQ(session->delegate.did_disconnect_devtools_client_call_count, 0u);
}

TEST_CASE(inspector_walker_highlighter_layout_and_editing)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto inspector_actor = actor_from(target, "inspectorActor"sv);
    auto walker = get_walker(client, inspector_actor);
    auto walker_actor = actor_from(walker, "actor"sv);
    auto root_node = walker.get_object("root"sv).release_value();
    auto root_node_actor = actor_from(root_node, "actor"sv);

    EXPECT_EQ(session->delegate.inspect_tab_call_count, 1u);
    EXPECT(root_node.get_bool("isTopLevelDocument"sv).value());
    EXPECT_EQ(root_node.get_string("displayType"sv).value(), "block"sv);

    JsonObject children;
    children.set("to"sv, walker_actor);
    children.set("type"sv, "children"sv);
    children.set("node"sv, root_node_actor);
    auto children_response = client.request(move(children));
    EXPECT(children_response.get_bool("hasFirst"sv).value());
    EXPECT_EQ(children_response.get_array("nodes"sv)->size(), 1u);

    auto div_actor = query_selector(client, walker_actor, root_node_actor, "div"sv);
    auto section_actor = query_selector(client, walker_actor, root_node_actor, "section"sv);
    auto span_actor = query_selector(client, walker_actor, root_node_actor, "span"sv);
    auto flex_actor = query_selector(client, walker_actor, root_node_actor, "aside"sv);
    auto first_flex_item_actor = query_selector(client, walker_actor, root_node_actor, "p"sv);
    JsonObject previous_sibling;
    previous_sibling.set("to"sv, walker_actor);
    previous_sibling.set("type"sv, "previousSibling"sv);
    previous_sibling.set("node"sv, div_actor);
    EXPECT_EQ(client.request(move(previous_sibling)).get_object("node"sv)->get_string("displayName"sv).value(), "span"sv);

    JsonObject is_in_dom_tree;
    is_in_dom_tree.set("to"sv, walker_actor);
    is_in_dom_tree.set("type"sv, "isInDOMTree"sv);
    is_in_dom_tree.set("node"sv, div_actor);
    EXPECT(client.request(move(is_in_dom_tree)).get_bool("attached"sv).value());

    JsonObject watch_root;
    watch_root.set("to"sv, walker_actor);
    watch_root.set("type"sv, "watchRootNode"sv);
    EXPECT_EQ(client.request(move(watch_root)).get_string("type"sv).value(), "root-available"sv);
    while (true) {
        auto message = client.read_message();
        if (message.get_string("from"sv).value_or({}) == walker_actor && !message.has_string("type"sv))
            break;
    }

    EXPECT(client.request(walker_actor, "retainNode"sv).has_string("from"sv));
    EXPECT(client.request(walker_actor, "getOffsetParent"sv).get("node"sv).value().is_null());

    JsonObject highlighter_request;
    highlighter_request.set("to"sv, inspector_actor);
    highlighter_request.set("type"sv, "getHighlighterByType"sv);
    highlighter_request.set("typeName"sv, "BoxModelHighlighter"sv);
    auto highlighter_actor = client.request(move(highlighter_request)).get_object("highlighter"sv)->get_string("actor"sv).release_value();

    JsonObject second_highlighter_request;
    second_highlighter_request.set("to"sv, inspector_actor);
    second_highlighter_request.set("type"sv, "getHighlighterByType"sv);
    second_highlighter_request.set("typeName"sv, "BoxModelHighlighter"sv);
    auto second_highlighter_response = client.request(move(second_highlighter_request));
    VERIFY(second_highlighter_response.has_object("highlighter"sv));
    EXPECT_NE(
        second_highlighter_response.get_object("highlighter"sv)->get_string("actor"sv).value(),
        highlighter_actor);

    JsonObject show_highlighter;
    show_highlighter.set("to"sv, highlighter_actor);
    show_highlighter.set("type"sv, "show"sv);
    show_highlighter.set("node"sv, div_actor);
    EXPECT(client.request(move(show_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.highlight_dom_node_call_count, 1u);
    EXPECT_EQ(session->delegate.last_highlighted_dom_node.value(), 4u);

    JsonObject show_unknown;
    show_unknown.set("to"sv, highlighter_actor);
    show_unknown.set("type"sv, "show"sv);
    show_unknown.set("node"sv, "missing-actor"sv);
    EXPECT(!client.request(move(show_unknown)).get_bool("value"sv).value());
    EXPECT_EQ(client.request(highlighter_actor, "hide"sv).get_string("from"sv).value(), highlighter_actor);

    JsonObject grid_highlighter_request;
    grid_highlighter_request.set("to"sv, inspector_actor);
    grid_highlighter_request.set("type"sv, "getHighlighterByType"sv);
    grid_highlighter_request.set("typeName"sv, "CssGridHighlighter"sv);
    auto grid_highlighter_actor = client.request(move(grid_highlighter_request)).get_object("highlighter"sv)->get_string("actor"sv).release_value();

    JsonObject second_grid_highlighter_request;
    second_grid_highlighter_request.set("to"sv, inspector_actor);
    second_grid_highlighter_request.set("type"sv, "getHighlighterByType"sv);
    second_grid_highlighter_request.set("typeName"sv, "CssGridHighlighter"sv);
    auto second_grid_highlighter_actor = client.request(move(second_grid_highlighter_request))
                                             .get_object("highlighter"sv)
                                             ->get_string("actor"sv)
                                             .release_value();
    EXPECT_NE(second_grid_highlighter_actor, grid_highlighter_actor);

    JsonObject grid_options;
    grid_options.set("showGridArea"sv, true);

    JsonObject show_grid_highlighter;
    show_grid_highlighter.set("to"sv, grid_highlighter_actor);
    show_grid_highlighter.set("type"sv, "show"sv);
    show_grid_highlighter.set("node"sv, div_actor);
    show_grid_highlighter.set("options"sv, move(grid_options));
    EXPECT(client.request(move(show_grid_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.highlight_grid_call_count, 1u);
    EXPECT_EQ(session->delegate.last_highlighted_grid_node.value(), 4u);
    EXPECT(session->delegate.last_grid_highlight_options.as_object().get_bool("showGridArea"sv).value());
    EXPECT_EQ(session->delegate.clear_grid_highlight_call_count, 0u);

    JsonObject show_second_grid_highlighter;
    show_second_grid_highlighter.set("to"sv, second_grid_highlighter_actor);
    show_second_grid_highlighter.set("type"sv, "show"sv);
    show_second_grid_highlighter.set("node"sv, span_actor);
    EXPECT(client.request(move(show_second_grid_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.clear_grid_highlight_call_count, 0u);
    EXPECT_EQ(session->delegate.highlight_grid_call_count, 2u);
    EXPECT_EQ(session->delegate.last_highlighted_grid_node.value(), 8u);

    EXPECT_EQ(client.request(grid_highlighter_actor, "hide"sv).get_string("from"sv).value(), grid_highlighter_actor);
    EXPECT_EQ(session->delegate.clear_grid_highlight_call_count, 1u);
    EXPECT_EQ(session->delegate.last_cleared_grid_node.value(), 4u);

    JsonObject retarget_second_grid_highlighter;
    retarget_second_grid_highlighter.set("to"sv, second_grid_highlighter_actor);
    retarget_second_grid_highlighter.set("type"sv, "show"sv);
    retarget_second_grid_highlighter.set("node"sv, div_actor);
    EXPECT(client.request(move(retarget_second_grid_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.clear_grid_highlight_call_count, 2u);
    EXPECT_EQ(session->delegate.last_cleared_grid_node.value(), 8u);
    EXPECT_EQ(session->delegate.highlight_grid_call_count, 3u);
    EXPECT_EQ(session->delegate.last_highlighted_grid_node.value(), 4u);

    EXPECT_EQ(client.request(second_grid_highlighter_actor, "release"sv).get_string("from"sv).value(), second_grid_highlighter_actor);
    EXPECT_EQ(session->delegate.clear_grid_highlight_call_count, 3u);
    EXPECT_EQ(session->delegate.last_cleared_grid_node.value(), 4u);

    JsonObject flexbox_highlighter_request;
    flexbox_highlighter_request.set("to"sv, inspector_actor);
    flexbox_highlighter_request.set("type"sv, "getHighlighterByType"sv);
    flexbox_highlighter_request.set("typeName"sv, "FlexboxHighlighter"sv);
    auto flexbox_highlighter_actor = client.request(move(flexbox_highlighter_request)).get_object("highlighter"sv)->get_string("actor"sv).release_value();

    JsonObject second_flexbox_highlighter_request;
    second_flexbox_highlighter_request.set("to"sv, inspector_actor);
    second_flexbox_highlighter_request.set("type"sv, "getHighlighterByType"sv);
    second_flexbox_highlighter_request.set("typeName"sv, "FlexboxHighlighter"sv);
    auto second_flexbox_highlighter_actor = client.request(move(second_flexbox_highlighter_request))
                                                .get_object("highlighter"sv)
                                                ->get_string("actor"sv)
                                                .release_value();
    EXPECT_NE(second_flexbox_highlighter_actor, flexbox_highlighter_actor);

    JsonObject flexbox_options;
    flexbox_options.set("color"sv, "#9400ff"sv);

    JsonObject show_flexbox_highlighter;
    show_flexbox_highlighter.set("to"sv, flexbox_highlighter_actor);
    show_flexbox_highlighter.set("type"sv, "show"sv);
    show_flexbox_highlighter.set("node"sv, flex_actor);
    show_flexbox_highlighter.set("options"sv, move(flexbox_options));
    EXPECT(client.request(move(show_flexbox_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.highlight_flexbox_call_count, 1u);
    EXPECT_EQ(session->delegate.last_highlighted_flexbox_node.value(), 10u);
    EXPECT_EQ(session->delegate.last_flexbox_highlight_options.as_object().get_string("color"sv).value(), "#9400ff"sv);
    EXPECT_EQ(session->delegate.clear_flexbox_highlight_call_count, 0u);

    JsonObject show_second_flexbox_highlighter;
    show_second_flexbox_highlighter.set("to"sv, second_flexbox_highlighter_actor);
    show_second_flexbox_highlighter.set("type"sv, "show"sv);
    show_second_flexbox_highlighter.set("node"sv, first_flex_item_actor);
    EXPECT(client.request(move(show_second_flexbox_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.clear_flexbox_highlight_call_count, 0u);
    EXPECT_EQ(session->delegate.highlight_flexbox_call_count, 2u);
    EXPECT_EQ(session->delegate.last_highlighted_flexbox_node.value(), 11u);

    EXPECT_EQ(client.request(flexbox_highlighter_actor, "hide"sv).get_string("from"sv).value(), flexbox_highlighter_actor);
    EXPECT_EQ(session->delegate.clear_flexbox_highlight_call_count, 1u);
    EXPECT_EQ(session->delegate.last_cleared_flexbox_node.value(), 10u);

    JsonObject retarget_second_flexbox_highlighter;
    retarget_second_flexbox_highlighter.set("to"sv, second_flexbox_highlighter_actor);
    retarget_second_flexbox_highlighter.set("type"sv, "show"sv);
    retarget_second_flexbox_highlighter.set("node"sv, flex_actor);
    EXPECT(client.request(move(retarget_second_flexbox_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.clear_flexbox_highlight_call_count, 2u);
    EXPECT_EQ(session->delegate.last_cleared_flexbox_node.value(), 11u);
    EXPECT_EQ(session->delegate.highlight_flexbox_call_count, 3u);
    EXPECT_EQ(session->delegate.last_highlighted_flexbox_node.value(), 10u);

    EXPECT_EQ(client.request(second_flexbox_highlighter_actor, "release"sv).get_string("from"sv).value(), second_flexbox_highlighter_actor);
    EXPECT_EQ(session->delegate.clear_flexbox_highlight_call_count, 3u);
    EXPECT_EQ(session->delegate.last_cleared_flexbox_node.value(), 10u);

    auto layout_actor = client.request(walker_actor, "getLayoutInspector"sv).get_object("actor"sv)->get_string("actor"sv).release_value();

    JsonObject get_grids;
    get_grids.set("to"sv, layout_actor);
    get_grids.set("type"sv, "getGrids"sv);
    get_grids.set("rootNode"sv, root_node_actor);
    auto grids = client.request(move(get_grids)).get_array("grids"sv).release_value();
    EXPECT_EQ(session->delegate.inspect_grid_layouts_call_count, 1u);
    EXPECT_EQ(session->delegate.last_grid_root_node.value(), 1u);
    EXPECT_EQ(grids.size(), 3u);

    auto const& content_grid = grids.at(0).as_object();
    EXPECT(!content_grid.has("containerNodeId"sv));
    EXPECT(content_grid.has_string("actor"sv));
    EXPECT_EQ(content_grid.get_string("containerNodeActorID"sv).value(), div_actor);
    EXPECT_EQ(content_grid.get_string("direction"sv).value(), "ltr"sv);
    EXPECT_EQ(content_grid.get_string("writingMode"sv).value(), "horizontal-tb"sv);
    EXPECT_EQ(content_grid.get_bool("isSubgrid"sv).value(), false);
    auto const& content_grid_fragment = content_grid.get_array("gridFragments"sv)->at(0).as_object();
    EXPECT_EQ(content_grid_fragment.get_array("areas"sv)->at(0).as_object().get_string("name"sv).value(), "content"sv);
    EXPECT_EQ(content_grid_fragment.get_object("cols"sv)->get_array("lines"sv)->at(0).as_object().get_integer<i32>("negativeNumber"sv).value(), -2);

    auto const& subgrid = grids.at(1).as_object();
    EXPECT_EQ(subgrid.get_string("containerNodeActorID"sv).value(), section_actor);
    EXPECT_EQ(subgrid.get_bool("isSubgrid"sv).value(), true);
    EXPECT_EQ(subgrid.get_array("gridFragments"sv)->at(0).as_object().get_array("areas"sv)->at(0).as_object().get_string("name"sv).value(), "subgrid"sv);

    JsonObject get_parent_grid_node;
    get_parent_grid_node.set("to"sv, walker_actor);
    get_parent_grid_node.set("type"sv, "getParentGridNode"sv);
    get_parent_grid_node.set("node"sv, section_actor);
    EXPECT_EQ(client.request(move(get_parent_grid_node)).get_object("node"sv)->get_string("actor"sv).value(), div_actor);

    JsonObject get_missing_parent_grid_node;
    get_missing_parent_grid_node.set("to"sv, walker_actor);
    get_missing_parent_grid_node.set("type"sv, "getParentGridNode"sv);
    get_missing_parent_grid_node.set("node"sv, div_actor);
    EXPECT(client.request(move(get_missing_parent_grid_node)).get("node"sv).value().is_null());

    auto const& subframe_grid = grids.at(2).as_object();
    EXPECT_EQ(subframe_grid.get_string("containerNodeActorID"sv).value(), span_actor);
    EXPECT_EQ(subframe_grid.get_array("gridFragments"sv)->at(0).as_object().get_array("areas"sv)->at(0).as_object().get_string("name"sv).value(), "subframe"sv);

    JsonObject get_current_grid;
    get_current_grid.set("to"sv, layout_actor);
    get_current_grid.set("type"sv, "getCurrentGrid"sv);
    get_current_grid.set("node"sv, div_actor);
    auto current_grid = client.request(move(get_current_grid)).get_object("grid"sv).release_value();
    EXPECT_EQ(session->delegate.inspect_current_grid_call_count, 1u);
    EXPECT_EQ(session->delegate.last_current_grid_node.value(), 4u);
    EXPECT_EQ(current_grid.get_string("actor"sv).value(), content_grid.get_string("actor"sv).value());
    EXPECT_EQ(current_grid.get_string("containerNodeActorID"sv).value(), div_actor);

    JsonObject get_missing_flexbox;
    get_missing_flexbox.set("to"sv, layout_actor);
    get_missing_flexbox.set("type"sv, "getCurrentFlexbox"sv);
    get_missing_flexbox.set("node"sv, root_node_actor);
    EXPECT(client.request(move(get_missing_flexbox)).get("flexbox"sv).value().is_null());
    EXPECT_EQ(session->delegate.inspect_current_flexbox_call_count, 1u);
    EXPECT_EQ(session->delegate.last_current_flexbox_node.value(), 1u);
    EXPECT(!session->delegate.last_current_flexbox_only_look_at_parents);

    JsonObject get_current_flexbox;
    get_current_flexbox.set("to"sv, layout_actor);
    get_current_flexbox.set("type"sv, "getCurrentFlexbox"sv);
    get_current_flexbox.set("node"sv, flex_actor);
    auto current_flexbox = client.request(move(get_current_flexbox)).get_object("flexbox"sv).release_value();
    EXPECT_EQ(session->delegate.inspect_current_flexbox_call_count, 2u);
    EXPECT_EQ(session->delegate.last_current_flexbox_node.value(), 10u);
    EXPECT_EQ(current_flexbox.get_string("containerNodeActorID"sv).value(), flex_actor);
    EXPECT(!current_flexbox.has("containerNodeId"sv));
    EXPECT(!current_flexbox.has("items"sv));
    EXPECT_EQ(current_flexbox.get_object("properties"sv)->get_string("flex-direction"sv).value(), "row"sv);
    auto flexbox_actor = current_flexbox.get_string("actor"sv).release_value();

    JsonObject get_parent_flexbox;
    get_parent_flexbox.set("to"sv, layout_actor);
    get_parent_flexbox.set("type"sv, "getCurrentFlexbox"sv);
    get_parent_flexbox.set("node"sv, flex_actor);
    get_parent_flexbox.set("onlyLookAtParents"sv, true);
    EXPECT(client.request(move(get_parent_flexbox)).get("flexbox"sv).value().is_null());
    EXPECT_EQ(session->delegate.inspect_current_flexbox_call_count, 3u);
    EXPECT(session->delegate.last_current_flexbox_only_look_at_parents);

    JsonObject get_flex_item_parent_flexbox;
    get_flex_item_parent_flexbox.set("to"sv, layout_actor);
    get_flex_item_parent_flexbox.set("type"sv, "getCurrentFlexbox"sv);
    get_flex_item_parent_flexbox.set("node"sv, first_flex_item_actor);
    get_flex_item_parent_flexbox.set("onlyLookAtParents"sv, true);
    auto parent_flexbox = client.request(move(get_flex_item_parent_flexbox)).get_object("flexbox"sv).release_value();
    EXPECT_EQ(session->delegate.inspect_current_flexbox_call_count, 4u);
    EXPECT_EQ(session->delegate.last_current_flexbox_node.value(), 11u);
    EXPECT_EQ(parent_flexbox.get_string("actor"sv).value(), flexbox_actor);

    JsonObject get_flex_items;
    get_flex_items.set("to"sv, flexbox_actor);
    get_flex_items.set("type"sv, "getFlexItems"sv);
    auto flex_items = client.request(move(get_flex_items)).get_array("flexitems"sv).release_value();
    EXPECT_EQ(flex_items.size(), 2u);
    auto const& first_flex_item = flex_items.at(0).as_object();
    EXPECT(first_flex_item.has_string("actor"sv));
    EXPECT(!first_flex_item.has("nodeId"sv));
    EXPECT_EQ(first_flex_item.get_string("nodeActorID"sv).value(), first_flex_item_actor);
    EXPECT_EQ(first_flex_item.get_object("flexItemSizing"sv)->get_string("lineGrowthState"sv).value(), "growing"sv);
    EXPECT_EQ(first_flex_item.get_object("flexItemSizing"sv)->get_string("clampState"sv).value(), "unclamped"sv);
    EXPECT_EQ(first_flex_item.get_object("properties"sv)->get_string("flex-basis"sv).value(), "auto"sv);
    EXPECT_EQ(first_flex_item.get_object("computedStyle"sv)->get_integer<int>("flexGrow"sv).value(), 1);

    auto const& second_flex_item = flex_items.at(1).as_object();
    EXPECT_EQ(second_flex_item.get_object("flexItemSizing"sv)->get_string("lineGrowthState"sv).value(), "shrinking"sv);
    EXPECT_EQ(second_flex_item.get_object("flexItemSizing"sv)->get_string("clampState"sv).value(), "clamped_to_min"sv);

    JsonObject node_from_flexbox;
    node_from_flexbox.set("to"sv, walker_actor);
    node_from_flexbox.set("type"sv, "getNodeFromActor"sv);
    node_from_flexbox.set("actorID"sv, flexbox_actor);
    JsonArray flexbox_path;
    flexbox_path.must_append("containerEl"sv);
    node_from_flexbox.set("path"sv, move(flexbox_path));
    EXPECT_EQ(client.request(move(node_from_flexbox)).get_object("node"sv)->get_object("node"sv)->get_string("actor"sv).value(), flex_actor);

    JsonObject node_from_flex_item;
    node_from_flex_item.set("to"sv, walker_actor);
    node_from_flex_item.set("type"sv, "getNodeFromActor"sv);
    node_from_flex_item.set("actorID"sv, first_flex_item.get_string("actor"sv).value());
    JsonArray flex_item_path;
    flex_item_path.must_append("element"sv);
    node_from_flex_item.set("path"sv, move(flex_item_path));
    EXPECT_EQ(client.request(move(node_from_flex_item)).get_object("node"sv)->get_object("node"sv)->get_string("actor"sv).value(), first_flex_item_actor);

    JsonObject set_outer_html;
    set_outer_html.set("to"sv, walker_actor);
    set_outer_html.set("type"sv, "setOuterHTML"sv);
    set_outer_html.set("node"sv, div_actor);
    set_outer_html.set("value"sv, "<section></section>"sv);
    EXPECT(client.request(move(set_outer_html)).has_string("from"sv));
    EXPECT_EQ(session->delegate.set_dom_node_outer_html_call_count, 1u);
    EXPECT_EQ(session->delegate.last_html.value(), "<section></section>"sv);

    JsonObject edit_tag;
    edit_tag.set("to"sv, walker_actor);
    edit_tag.set("type"sv, "editTagName"sv);
    edit_tag.set("node"sv, div_actor);
    edit_tag.set("tagName"sv, "section"sv);
    EXPECT(client.request(move(edit_tag)).has_string("from"sv));
    EXPECT_EQ(session->delegate.set_dom_node_tag_call_count, 1u);

    JsonObject duplicate;
    duplicate.set("to"sv, walker_actor);
    duplicate.set("type"sv, "duplicateNode"sv);
    duplicate.set("node"sv, div_actor);
    EXPECT(client.request(move(duplicate)).has_string("from"sv));
    EXPECT_EQ(session->delegate.clone_dom_node_call_count, 1u);

    JsonObject mutation_target = make_node(4, "element"sv, "DIV"sv);
    JsonObject attributes;
    attributes.set("id"sv, "target"sv);
    attributes.set("class"sv, "updated"sv);
    mutation_target.set("attributes"sv, move(attributes));

    WebView::Mutation mutation { "attributes"_string, 4, mutation_target.serialized(), WebView::AttributeMutation { "class"_string, "updated"_string } };
    session->delegate.emit_mutation(move(mutation));
    EXPECT_EQ(client.read_message().get_string("type"sv).value(), "newMutations"sv);

    auto mutations = client.request(walker_actor, "getMutations"sv).get_array("mutations"sv).release_value();
    VERIFY(mutations.size() == 1u);
    EXPECT_EQ(mutations.at(0).as_object().get_string("type"sv).value(), "attributes"sv);
    EXPECT_EQ(mutations.at(0).as_object().get_string("newValue"sv).value(), "updated"sv);
}

TEST_CASE(styles_and_stylesheets)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto inspector_actor = actor_from(target, "inspectorActor"sv);
    auto style_sheets_actor = actor_from(target, "styleSheetsActor"sv);

    auto page_style = client.request(inspector_actor, "getPageStyle"sv).get_object("pageStyle"sv).release_value();
    auto page_style_actor = actor_from(page_style, "actor"sv);
    EXPECT(page_style.get_object("traits"sv)->get_bool("fontVariations"sv).value());

    auto walker = get_walker(client, inspector_actor);
    auto walker_actor = actor_from(walker, "actor"sv);
    auto root_actor = walker.get_object("root"sv)->get_string("actor"sv).release_value();
    auto div_actor = query_selector(client, walker_actor, root_actor, "div"sv);

    auto get_applied = [&] {
        JsonObject applied;
        applied.set("to"sv, page_style_actor);
        applied.set("type"sv, "getApplied"sv);
        applied.set("node"sv, div_actor);
        applied.set("inherited"sv, true);
        applied.set("matchedSelectors"sv, true);
        return client.request(move(applied)).get_array("entries"sv).release_value();
    };

    auto applied_entries = get_applied();
    EXPECT(session->delegate.last_inspected_dom_node_options.get_bool("inherited"sv).value());
    EXPECT(session->delegate.last_inspected_dom_node_options.get_bool("matchedSelectors"sv).value());
    VERIFY(applied_entries.size() == 3u);
    EXPECT_EQ(style_rule_actor_count(*session->server), applied_entries.size());

    auto inline_rule = applied_entries.at(0).as_object().get_object("rule"sv).release_value();
    auto inline_rule_actor = actor_from(inline_rule, "actor"sv);
    EXPECT_EQ(inline_rule.get_integer<int>("type"sv).value(), 100);
    EXPECT(inline_rule.get_array("ancestorData"sv)->is_empty());
    EXPECT(!inline_rule.get_object("traits"sv)->get_bool("canSetRuleText"sv).value());
    EXPECT_EQ(inline_rule.get_array("declarations"sv)->at(0).as_object().get_string("name"sv).value(), "display"sv);
    EXPECT(inline_rule.get_array("declarations"sv)->at(0).as_object().get_bool("isNameValid"sv).value());
    EXPECT(inline_rule.get_array("declarations"sv)->at(0).as_object().get_bool("isValid"sv).value());
    EXPECT_EQ(client.request(inline_rule_actor, "getRuleText"sv).get_string("text"sv).value(), "display: grid;"sv);

    auto inherited_rule_entry = applied_entries.at(1).as_object();
    auto inherited_rule = inherited_rule_entry.get_object("rule"sv).release_value();
    EXPECT_EQ(inherited_rule_entry.get_string("inherited"sv).value(), root_actor);
    EXPECT_EQ(inherited_rule.get_array("selectors"sv)->at(0).as_string(), "body div.fixture"sv);
    EXPECT_EQ(inherited_rule.get_array("declarations"sv)->at(0).as_object().get_string("name"sv).value(), "color"sv);
    EXPECT(inherited_rule.get_array("declarations"sv)->at(0).as_object().get_bool("isNameValid"sv).value());
    EXPECT(inherited_rule.get_array("declarations"sv)->at(0).as_object().get_bool("isValid"sv).value());
    EXPECT(inherited_rule.get_array("declarations"sv)->at(1).as_object().get_bool("isNameValid"sv).value());
    EXPECT(!inherited_rule.get_array("declarations"sv)->at(1).as_object().get_bool("isValid"sv).value());
    EXPECT_EQ(inherited_rule.get_string("parentStyleSheet"sv).value(), MUST(String::formatted("{}-stylesheet:0", style_sheets_actor)));
    EXPECT_EQ(inherited_rule.get_integer<int>("line"sv).value(), 4);
    EXPECT_EQ(inherited_rule.get_integer<int>("column"sv).value(), 9);
    EXPECT(!inherited_rule.has("styleSheet"sv));
    EXPECT_EQ(inherited_rule_entry.get_array("matchedSelectorIndexes"sv)->at(0).as_integer<int>(), 0);

    auto user_agent_rule_entry = applied_entries.at(2).as_object();
    auto user_agent_rule = user_agent_rule_entry.get_object("rule"sv).release_value();
    EXPECT(user_agent_rule_entry.get_bool("isSystem"sv).value());
    EXPECT_EQ(user_agent_rule.get_array("selectors"sv)->at(0).as_string(), "div"sv);
    EXPECT_EQ(user_agent_rule.get_string("parentStyleSheet"sv).value(), MUST(String::formatted("{}-stylesheet:1", style_sheets_actor)));
    EXPECT_EQ(user_agent_rule.get_integer<int>("line"sv).value(), 12);
    EXPECT_EQ(user_agent_rule.get_integer<int>("column"sv).value(), 5);
    EXPECT(!user_agent_rule.has("styleSheet"sv));
    EXPECT(!client.request(page_style_actor, "isPositionEditable"sv).get_bool("value"sv).value());

    auto second_applied_entries = get_applied();
    VERIFY(second_applied_entries.size() == applied_entries.size());
    spin_until(session->loop, [&] {
        return style_rule_actor_count(*session->server) == second_applied_entries.size()
            && !session->server->actor_registry().contains(inline_rule_actor);
    });

    JsonObject computed_request;
    computed_request.set("to"sv, page_style_actor);
    computed_request.set("type"sv, "getComputed"sv);
    computed_request.set("node"sv, div_actor);
    auto computed = client.request(move(computed_request)).get_object("computed"sv).release_value();
    EXPECT_EQ(computed.get_object("display"sv)->get_string("value"sv).value(), "block"sv);

    JsonObject layout_request;
    layout_request.set("to"sv, page_style_actor);
    layout_request.set("type"sv, "getLayout"sv);
    layout_request.set("node"sv, div_actor);
    auto layout = client.request(move(layout_request));
    EXPECT_EQ(layout.get_string("width"sv).value(), "100px"sv);
    EXPECT_EQ(layout.get_string("margin-top"sv).value(), "5px"sv);

    JsonObject font_request;
    font_request.set("to"sv, page_style_actor);
    font_request.set("type"sv, "getUsedFontFaces"sv);
    font_request.set("node"sv, div_actor);
    auto fonts = client.request(move(font_request)).get_array("fontFaces"sv).release_value();
    VERIFY(fonts.size() == 1u);
    EXPECT_EQ(fonts.at(0).as_object().get_string("name"sv).value(), "Test Sans"sv);

    auto style_resources = client.read_message();
    while (style_resources.get_string("type"sv).value_or({}) != "resources-available-array"sv)
        style_resources = client.read_message();
    auto sheets = style_resources.get_array("array"sv)->at(0).as_array().at(1).as_array();
    VERIFY(sheets.size() == 2u);
    auto resource_id = sheets.at(0).as_object().get_string("resourceId"sv).release_value();

    JsonObject get_text;
    get_text.set("to"sv, style_sheets_actor);
    get_text.set("type"sv, "getText"sv);
    get_text.set("resourceId"sv, resource_id);
    EXPECT_EQ(client.request(move(get_text)).get_string("text"sv).value(), "body { color: red; }"sv);

    JsonObject bad_get_text;
    bad_get_text.set("to"sv, style_sheets_actor);
    bad_get_text.set("type"sv, "getText"sv);
    bad_get_text.set("resourceId"sv, "missing:99"sv);
    EXPECT_EQ(client.request(move(bad_get_text)).get_string("error"sv).value(), "unknownActor"sv);
}

TEST_CASE(devtools_server_teardown_with_pending_actor_cleanup)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto tab_actor = actor_from(get_tab(client), "actor"sv);
    session->server->unregister_actor(tab_actor);
    session->server->connection()->on_connection_closed();
    session->server.clear();
    pump(session->loop);
}

TEST_CASE(console_network_navigation_and_accessibility)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto accessibility_actor = actor_from(target, "accessibilityActor"sv);

    session->delegate.emit_console_log(JS::Console::LogLevel::Warn, { "careful"_string });
    auto warning = read_resource(client, "console-message"sv);
    EXPECT_EQ(warning.get_string("level"sv).value(), "warn"sv);

    session->delegate.emit_console_trace();
    auto trace = read_resource(client, "console-message"sv);
    EXPECT_EQ(trace.get_string("level"sv).value(), "trace"sv);
    EXPECT_EQ(trace.get_array("stacktrace"sv)->at(0).as_object().get_string("functionName"sv).value(), "traceFunction"sv);

    session->delegate.emit_console_error();
    auto page_error = read_resource(client, "error-message"sv).get_object("pageError"sv).release_value();
    EXPECT(page_error.get_bool("isPromiseRejection"sv).value());

    session->delegate.emit_network_lifecycle();
    auto network_event = read_resource(client, "network-event"sv);
    EXPECT_EQ(network_event.get_string("method"sv).value(), "POST"sv);
    EXPECT(network_event.get_bool("isXHR"sv).value());
    auto network_actor = network_event.get_string("actor"sv).release_value();
    (void)read_resource(client, "network-event"sv, "resources-updated-array"sv);
    (void)read_resource(client, "network-event"sv, "resources-updated-array"sv);

    JsonObject content_request;
    content_request.set("to"sv, network_actor);
    content_request.set("type"sv, "getResponseContent"sv);
    auto content = client.request(move(content_request)).get_object("content"sv).release_value();
    EXPECT_EQ(content.get_string("text"sv).value(), "{\"ok\":true}"sv);
    EXPECT_EQ(content.get_string("mimeType"sv).value(), "application/json"sv);

    session->delegate.emit_navigation();
    EXPECT_EQ(read_resource(client, "document-event"sv).get_string("name"sv).value(), "will-navigate"sv);

    (void)read_packet_with_type(client, "target-destroyed-form"sv);
    auto new_target_available = read_packet_with_type(client, "target-available-form"sv);
    target = new_target_available.get_object("target"sv).release_value();
    accessibility_actor = actor_from(target, "accessibilityActor"sv);

    EXPECT_EQ(client.request(actor_from(target, "actor"sv), "listFrames"sv).get_string("from"sv).value(), actor_from(target, "actor"sv));

    EXPECT_EQ(read_resource(client, "document-event"sv).get_string("name"sv).value(), "dom-loading"sv);
    EXPECT_EQ(read_resource(client, "document-event"sv).get_string("name"sv).value(), "dom-interactive"sv);
    EXPECT_EQ(read_resource(client, "document-event"sv).get_string("name"sv).value(), "dom-complete"sv);

    EXPECT(client.request(accessibility_actor, "bootstrap"sv).has_object("state"sv));
    EXPECT(client.request(accessibility_actor, "getTraits"sv).get_object("traits"sv)->get_bool("tabbingOrder"sv).value());
    EXPECT(client.request(accessibility_actor, "getSimulator"sv).get("simulator"sv).value().is_null());

    auto accessibility_walker = client.request(accessibility_actor, "getWalker"sv).get_object("walker"sv)->get_string("actor"sv).release_value();
    auto accessibility_root = client.request(accessibility_walker, "children"sv).get_array("children"sv)->at(0).as_object().get_string("actor"sv).release_value();
    auto accessibility_button = client.request(accessibility_root, "children"sv).get_array("children"sv)->at(0).as_object().get_string("actor"sv).release_value();

    auto walker = get_walker(client, actor_from(target, "inspectorActor"sv));
    JsonObject node_from_accessibility;
    node_from_accessibility.set("to"sv, actor_from(walker, "actor"sv));
    node_from_accessibility.set("type"sv, "getNodeFromActor"sv);
    node_from_accessibility.set("actorID"sv, accessibility_button);
    JsonArray path;
    path.must_append("rawAccessible"sv);
    path.must_append("DOMNode"sv);
    node_from_accessibility.set("path"sv, move(path));
    auto node_response = client.request(move(node_from_accessibility));
    EXPECT_EQ(node_response.get_object("node"sv)->get_object("node"sv)->get_string("displayName"sv).value(), "div"sv);
}
