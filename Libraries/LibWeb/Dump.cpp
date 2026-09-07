/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/CSS/CSSDescriptors.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSPageRule.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web {

static void dump_session_history_entry(StringBuilder& builder, HTML::SessionHistoryEntry const& session_history_entry)
{
    builder.appendff("step=({}) url=({})", session_history_entry.step().get<int>(), session_history_entry.url());
    if (session_history_entry.scroll_position_data().viewport_scroll_position.has_value()) {
        auto const& viewport_scroll_position = *session_history_entry.scroll_position_data().viewport_scroll_position;
        builder.appendff(" viewport-scroll=({}, {})", viewport_scroll_position.x(), viewport_scroll_position.y());
    }
    builder.append('\n');
}

void dump_tree(HTML::LocalNavigable& navigable)
{
    StringBuilder builder;
    if (auto window = navigable.active_window()) {
        for (auto const& entry : window->navigation()->entries())
            dump_session_history_entry(builder, entry->session_history_entry());
    }
    dbgln("{}", builder.string_view());
}

void dump_tree(DOM::Node const& node)
{
    StringBuilder builder;
    dump_tree(builder, node);
    dbgln("{}", builder.string_view());
}

void dump_tree(StringBuilder& builder, DOM::Node const& node)
{
    static int indent = 0;
    for (int i = 0; i < indent; ++i)
        builder.append("  "sv);
    if (auto const* element = as_if<DOM::Element>(node)) {
        auto namespace_prefix = [&] -> Utf16FlyString {
            auto const& namespace_uri = element->namespace_uri();
            if (!namespace_uri.has_value() || node.document().is_default_namespace(namespace_uri->view()))
                return ""_utf16_fly_string;
            if (namespace_uri == Namespace::HTML)
                return "html:"_utf16_fly_string;
            if (namespace_uri == Namespace::SVG)
                return "svg:"_utf16_fly_string;
            if (namespace_uri == Namespace::MathML)
                return "mathml:"_utf16_fly_string;
            return *namespace_uri;
        }();

        builder.appendff("<{}{}", namespace_prefix, element->local_name());
        element->for_each_attribute([&](Utf16FlyString const& name, Utf16View value) {
            builder.appendff(" {}={}", name, value);
        });
        builder.append(">\n"sv);
        if (element->associated_shadow_host_pseudo_element().has_value()) {
            for (int i = 0; i < indent; ++i)
                builder.append("  "sv);
            builder.appendff("  (pseudo-element: {})\n", CSS::pseudo_element_name(element->associated_shadow_host_pseudo_element().value()));
        }
    } else if (auto const* text = as_if<DOM::Text>(node)) {
        builder.appendff("\"{}\"\n", text->data());
    } else {
        builder.appendff("{}\n", node.node_name());
    }
    ++indent;
    if (auto const* element = as_if<DOM::Element>(node); element && element->shadow_root())
        dump_tree(builder, *element->shadow_root());
    if (auto const* image = as_if<HTML::HTMLImageElement>(node)) {
        if (auto const* svg_data = as_if<SVG::SVGDecodedImageData>(image->current_request().image_data().ptr())) {
            ++indent;
            for (int i = 0; i < indent; ++i)
                builder.append("  "sv);
            builder.append("(SVG-as-image isolated context)\n"sv);
            dump_tree(builder, svg_data->svg_document());
            --indent;
        }
    }
    if (auto const* template_element = as_if<HTML::HTMLTemplateElement>(node)) {
        for (int i = 0; i < indent; ++i)
            builder.append("  "sv);
        builder.append("(template content)\n"sv);
        dump_tree(builder, template_element->content());
        builder.append("(template normal subtree)\n"sv);
    }
    if (auto const* parent_node = as_if<DOM::ParentNode>(node)) {
        parent_node->for_each_child([&](auto const& child) {
            dump_tree(builder, child);
            return IterationDecision::Continue;
        });
    }
    --indent;
}

void dump_tree(Layout::Node const& layout_node)
{
    StringBuilder builder;
    dump_tree(builder, layout_node, true);
    dbgln("{}", builder.string_view());
}

void dump_tree(StringBuilder& builder, Layout::Node const& layout_node, bool interactive)
{
    Painting::dump_layout_tree(builder, layout_node, interactive);
}

void dump_selector(CSS::Selector const& selector)
{
    StringBuilder builder;
    dump_selector(builder, selector);
    dbgln("{}", builder.string_view());
}

void dump_selector(StringBuilder& builder, CSS::Selector const& selector, int indent_levels)
{
    dump_serialized_selector(builder, selector.serialize().to_utf8(), indent_levels);
}
void dump_rule(CSS::CSSRule const& rule)
{
    StringBuilder builder;
    dump_rule(builder, rule);
    dbgln("{}", builder.string_view());
}

void dump_rule(StringBuilder& builder, CSS::CSSRule const& rule, int indent_levels)
{
    rule.dump(builder, indent_levels);
}

void dump_style_properties(StringBuilder& builder, CSS::CSSStyleProperties const& declaration, int indent_levels)
{
    dump_indent(builder, indent_levels);
    builder.appendff("Declarations ({}):\n", declaration.length());
    for (auto& property : declaration.properties()) {
        dump_indent(builder, indent_levels);
        builder.appendff("  {}: '{}'", CSS::string_from_property_id(property.property_id), property.value->to_string(CSS::SerializationMode::Normal));
        if (property.important == CSS::Important::Yes)
            builder.append(" \033[31;1m!important\033[0m"sv);
        builder.append('\n');
    }
    for (auto& property : declaration.custom_properties()) {
        dump_indent(builder, indent_levels);
        builder.appendff("  {}: '{}'", property.key, property.value.value->to_string(CSS::SerializationMode::Normal));
        if (property.value.important == CSS::Important::Yes)
            builder.append(" \033[31;1m!important\033[0m"sv);
        builder.append('\n');
    }
}

void dump_descriptors(StringBuilder& builder, CSS::CSSDescriptors const& descriptors, int indent_levels)
{
    dump_indent(builder, indent_levels);
    builder.appendff("Declarations ({}):\n", descriptors.length());
    for (auto const& descriptor : descriptors.descriptors()) {
        dump_indent(builder, indent_levels);
        builder.appendff("  {}: '{}'", descriptor.descriptor_name_and_id.name(), descriptor.value->to_string(CSS::SerializationMode::Normal));
        builder.append('\n');
    }
}

void dump_sheet(CSS::StyleSheetState const& sheet)
{
    StringBuilder builder;
    dump_sheet(builder, sheet);
    dbgln("{}", builder.string_view());
}

void dump_sheet(StringBuilder& builder, CSS::StyleSheetState const& sheet, int indent_levels)
{
    dump_indent(builder, indent_levels);
    builder.appendff("CSSStyleSheet{{{}}}: {} rule(s)\n", &sheet, sheet.rules().length());

    for (auto& rule : sheet.rules())
        dump_rule(builder, rule, indent_levels + 1);
}

}
