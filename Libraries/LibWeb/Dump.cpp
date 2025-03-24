/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/SVGBox.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <stdio.h>

namespace Web {

static void indent(StringBuilder& builder, int levels)
{
    for (int i = 0; i < levels; i++)
        builder.append("  "sv);
}

static void dump_session_history_entry(StringBuilder& builder, HTML::SessionHistoryEntry const& session_history_entry, int indent_levels)
{
    indent(builder, indent_levels);
    auto const& document = session_history_entry.document();
    builder.appendff("step=({}) url=({}) is-active=({})\n", session_history_entry.step().get<int>(), session_history_entry.url(), document && document->is_active());
    for (auto const& nested_history : session_history_entry.document_state()->nested_histories()) {
        for (auto const& nested_she : nested_history.entries) {
            dump_session_history_entry(builder, *nested_she, indent_levels + 1);
        }
    }
}

void dump_tree(HTML::TraversableNavigable& traversable)
{
    StringBuilder builder;
    for (auto const& she : traversable.session_history_entries()) {
        dump_session_history_entry(builder, *she, 0);
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
    if (is<DOM::Element>(node)) {
        builder.appendff("<{}", as<DOM::Element>(node).local_name());
        as<DOM::Element>(node).for_each_attribute([&](auto& name, auto& value) {
            builder.appendff(" {}={}", name, value);
        });
        builder.append(">\n"sv);
        auto& element = as<DOM::Element>(node);
        if (element.use_pseudo_element().has_value()) {
            for (int i = 0; i < indent; ++i)
                builder.append("  "sv);
            builder.appendff("  (pseudo-element: {})\n", CSS::pseudo_element_name(element.use_pseudo_element().value()));
        }
    } else if (is<DOM::Text>(node)) {
        builder.appendff("\"{}\"\n", as<DOM::Text>(node).data());
    } else {
        builder.appendff("{}\n", node.node_name());
    }
    ++indent;
    if (is<DOM::Element>(node)) {
        if (auto shadow_root = static_cast<DOM::Element const&>(node).shadow_root()) {
            dump_tree(builder, *shadow_root);
        }
    }
    if (is<HTML::HTMLImageElement>(node)) {
        if (auto image_data = static_cast<HTML::HTMLImageElement const&>(node).current_request().image_data()) {
            if (is<SVG::SVGDecodedImageData>(*image_data)) {
                ++indent;
                for (int i = 0; i < indent; ++i)
                    builder.append("  "sv);
                builder.append("(SVG-as-image isolated context)\n"sv);
                auto& svg_data = as<SVG::SVGDecodedImageData>(*image_data);
                dump_tree(builder, svg_data.svg_document());
                --indent;
            }
        }
    }
    if (is<DOM::ParentNode>(node)) {
        if (!is<HTML::HTMLTemplateElement>(node)) {
            static_cast<DOM::ParentNode const&>(node).for_each_child([&](auto& child) {
                dump_tree(builder, child);
                return IterationDecision::Continue;
            });
        } else {
            auto& template_element = as<HTML::HTMLTemplateElement>(node);
            dump_tree(builder, template_element.content());
        }
    }
    --indent;
}

void dump_tree(Layout::Node const& layout_node, bool show_box_model, bool show_cascaded_properties)
{
    StringBuilder builder;
    dump_tree(builder, layout_node, show_box_model, show_cascaded_properties, true);
    dbgln("{}", builder.string_view());
}

void dump_tree(StringBuilder& builder, Layout::Node const& layout_node, bool show_box_model, bool show_cascaded_properties, bool interactive)
{
    static size_t indent = 0;
    for (size_t i = 0; i < indent; ++i)
        builder.append("  "sv);

    FlyString tag_name;
    if (layout_node.is_anonymous())
        tag_name = "(anonymous)"_fly_string;
    else if (is<DOM::Element>(layout_node.dom_node()))
        tag_name = as<DOM::Element>(*layout_node.dom_node()).local_name();
    else
        tag_name = layout_node.dom_node()->node_name();

    String identifier;
    if (layout_node.dom_node() && is<DOM::Element>(*layout_node.dom_node())) {
        auto& element = as<DOM::Element>(*layout_node.dom_node());
        StringBuilder builder;
        if (element.id().has_value() && !element.id()->is_empty()) {
            builder.append('#');
            builder.append(*element.id());
        }
        for (auto& class_name : element.class_names()) {
            builder.append('.');
            builder.append(class_name);
        }
        identifier = MUST(builder.to_string());
    }

    StringView nonbox_color_on = ""sv;
    StringView box_color_on = ""sv;
    StringView svg_box_color_on = ""sv;
    StringView positioned_color_on = ""sv;
    StringView floating_color_on = ""sv;
    StringView inline_color_on = ""sv;
    StringView line_box_color_on = ""sv;
    StringView fragment_color_on = ""sv;
    StringView flex_color_on = ""sv;
    StringView table_color_on = ""sv;
    StringView formatting_context_color_on = ""sv;
    StringView color_off = ""sv;

    if (interactive) {
        nonbox_color_on = "\033[33m"sv;
        box_color_on = "\033[34m"sv;
        svg_box_color_on = "\033[31m"sv;
        positioned_color_on = "\033[31;1m"sv;
        floating_color_on = "\033[32;1m"sv;
        inline_color_on = "\033[36;1m"sv;
        line_box_color_on = "\033[34;1m"sv;
        fragment_color_on = "\033[35;1m"sv;
        flex_color_on = "\033[34;1m"sv;
        table_color_on = "\033[91;1m"sv;
        formatting_context_color_on = "\033[37;1m"sv;
        color_off = "\033[0m"sv;
    }

    auto dump_box_model = [&] {
        if (show_box_model && layout_node.first_paintable() && layout_node.first_paintable()->is_paintable_box()) {
            auto const& paintable_box = static_cast<Painting::PaintableBox const&>(*layout_node.first_paintable());
            auto const& box_model = paintable_box.box_model();
            // Dump the horizontal box properties
            builder.appendff(" [{}+{}+{} {} {}+{}+{}]",
                box_model.margin.left,
                box_model.border.left,
                box_model.padding.left,
                paintable_box.content_width(),
                box_model.padding.right,
                box_model.border.right,
                box_model.margin.right);

            // And the vertical box properties
            builder.appendff(" [{}+{}+{} {} {}+{}+{}]",
                box_model.margin.top,
                box_model.border.top,
                box_model.padding.top,
                paintable_box.content_height(),
                box_model.padding.bottom,
                box_model.border.bottom,
                box_model.margin.bottom);
        }
    };

    if (!is<Layout::Box>(layout_node)) {
        builder.appendff("{}{}{} <{}{}{}{}>",
            nonbox_color_on,
            layout_node.class_name(),
            color_off,
            tag_name,
            nonbox_color_on,
            identifier,
            color_off);

        dump_box_model();
    } else {
        auto& box = as<Layout::Box>(layout_node);
        StringView color_on = is<Layout::SVGBox>(box) ? svg_box_color_on : box_color_on;

        builder.appendff("{}{}{} <{}{}{}{}> ",
            color_on,
            box.class_name(),
            color_off,
            color_on,
            tag_name,
            color_off,
            identifier);

        if (auto const* paintable_box = box.paintable_box()) {
            builder.appendff("at ({},{}) content-size {}x{}",
                paintable_box->absolute_x(),
                paintable_box->absolute_y(),
                paintable_box->content_width(),
                paintable_box->content_height());
        } else {
            builder.appendff("(not painted)");
        }

        if (box.is_positioned())
            builder.appendff(" {}positioned{}", positioned_color_on, color_off);
        if (box.is_floating())
            builder.appendff(" {}floating{}", floating_color_on, color_off);
        if (box.is_inline_block())
            builder.appendff(" {}inline-block{}", inline_color_on, color_off);
        if (box.is_inline_table())
            builder.appendff(" {}inline-table{}", inline_color_on, color_off);
        if (box.display().is_flex_inside()) {
            StringView direction;
            switch (box.computed_values().flex_direction()) {
            case CSS::FlexDirection::Column:
                direction = "column"sv;
                break;
            case CSS::FlexDirection::ColumnReverse:
                direction = "column-reverse"sv;
                break;
            case CSS::FlexDirection::Row:
                direction = "row"sv;
                break;
            case CSS::FlexDirection::RowReverse:
                direction = "row-reverse"sv;
                break;
            }
            builder.appendff(" {}flex-container({}){}", flex_color_on, direction, color_off);
        }
        if (box.is_flex_item())
            builder.appendff(" {}flex-item{}", flex_color_on, color_off);
        if (box.display().is_table_inside())
            builder.appendff(" {}table-box{}", table_color_on, color_off);
        if (box.display().is_table_row_group())
            builder.appendff(" {}table-row-group{}", table_color_on, color_off);
        if (box.display().is_table_column_group())
            builder.appendff(" {}table-column-group{}", table_color_on, color_off);
        if (box.display().is_table_header_group())
            builder.appendff(" {}table-header-group{}", table_color_on, color_off);
        if (box.display().is_table_footer_group())
            builder.appendff(" {}table-footer-group{}", table_color_on, color_off);
        if (box.display().is_table_row())
            builder.appendff(" {}table-row{}", table_color_on, color_off);
        if (box.display().is_table_cell())
            builder.appendff(" {}table-cell{}", table_color_on, color_off);

        dump_box_model();

        if (auto formatting_context_type = Layout::FormattingContext::formatting_context_type_created_by_box(box); formatting_context_type.has_value()) {
            switch (formatting_context_type.value()) {
            case Layout::FormattingContext::Type::Block:
                builder.appendff(" [{}BFC{}]", formatting_context_color_on, color_off);
                break;
            case Layout::FormattingContext::Type::Flex:
                builder.appendff(" [{}FFC{}]", formatting_context_color_on, color_off);
                break;
            case Layout::FormattingContext::Type::Grid:
                builder.appendff(" [{}GFC{}]", formatting_context_color_on, color_off);
                break;
            case Layout::FormattingContext::Type::Table:
                builder.appendff(" [{}TFC{}]", formatting_context_color_on, color_off);
                break;
            case Layout::FormattingContext::Type::SVG:
                builder.appendff(" [{}SVG{}]", formatting_context_color_on, color_off);
                break;
            default:
                break;
            }
        }

        builder.appendff(" children: {}", box.children_are_inline() ? "inline" : "not-inline");

        if (is<Layout::NavigableContainerViewport>(box)) {
            auto const& frame_box = static_cast<Layout::NavigableContainerViewport const&>(box);
            if (auto const* document = frame_box.dom_node().content_document_without_origin_check()) {
                builder.appendff(" (url: {})", document->url());
                builder.append("\n"sv);
                if (auto const* nested_layout_root = document->layout_node()) {
                    ++indent;
                    dump_tree(builder, *nested_layout_root, show_box_model, show_cascaded_properties, interactive);
                    --indent;
                }
            }
        }
    }

    if (is<Layout::NodeWithStyleAndBoxModelMetrics>(layout_node)
        && static_cast<Layout::NodeWithStyleAndBoxModelMetrics const&>(layout_node).continuation_of_node())
        builder.append(" continuation"sv);

    builder.append("\n"sv);

    if (layout_node.dom_node() && is<HTML::HTMLImageElement>(*layout_node.dom_node())) {
        if (auto image_data = static_cast<HTML::HTMLImageElement const&>(*layout_node.dom_node()).current_request().image_data()) {
            if (is<SVG::SVGDecodedImageData>(*image_data)) {
                auto& svg_data = as<SVG::SVGDecodedImageData>(*image_data);
                if (svg_data.svg_document().layout_node()) {
                    ++indent;
                    for (size_t i = 0; i < indent; ++i)
                        builder.append("  "sv);
                    builder.append("(SVG-as-image isolated context)\n"sv);

                    dump_tree(builder, *svg_data.svg_document().layout_node(), show_box_model, show_cascaded_properties, interactive);
                    --indent;
                }
            }
        }
    }

    auto dump_fragment = [&](auto& fragment, size_t fragment_index) {
        for (size_t i = 0; i < indent; ++i)
            builder.append("  "sv);
        builder.appendff("  {}frag {}{} from {} ",
            fragment_color_on,
            fragment_index,
            color_off,
            fragment.layout_node().class_name());
        builder.appendff("start: {}, length: {}, rect: {} baseline: {}\n",
            fragment.start(),
            fragment.length(),
            fragment.absolute_rect(),
            fragment.baseline());
        if (is<Layout::TextNode>(fragment.layout_node())) {
            for (size_t i = 0; i < indent; ++i)
                builder.append("  "sv);
            auto const& layout_text = static_cast<Layout::TextNode const&>(fragment.layout_node());
            auto fragment_text = MUST(layout_text.text_for_rendering().substring_from_byte_offset(fragment.start(), fragment.length()));
            builder.appendff("      \"{}\"\n", fragment_text);
        }
    };

    if (is<Layout::BlockContainer>(layout_node) && static_cast<Layout::BlockContainer const&>(layout_node).children_are_inline()) {
        auto& block = static_cast<Layout::BlockContainer const&>(layout_node);
        for (size_t fragment_index = 0; block.paintable_with_lines() && fragment_index < block.paintable_with_lines()->fragments().size(); ++fragment_index) {
            auto const& fragment = block.paintable_with_lines()->fragments()[fragment_index];
            dump_fragment(fragment, fragment_index);
        }
    }

    if (is<Layout::InlineNode>(layout_node) && layout_node.first_paintable()) {
        auto const& inline_node = static_cast<Layout::InlineNode const&>(layout_node);
        for (auto const& paintable : inline_node.paintables()) {
            auto const& paintable_with_lines = static_cast<Painting::PaintableWithLines const&>(paintable);
            auto const& fragments = paintable_with_lines.fragments();
            for (size_t fragment_index = 0; fragment_index < fragments.size(); ++fragment_index) {
                auto const& fragment = fragments[fragment_index];
                dump_fragment(fragment, fragment_index);
            }
        }
    }

    if (show_cascaded_properties && layout_node.dom_node() && layout_node.dom_node()->is_element() && as<DOM::Element>(layout_node.dom_node())->computed_properties()) {
        struct NameAndValue {
            FlyString name;
            String value;
        };
        Vector<NameAndValue> properties;
        as<DOM::Element>(*layout_node.dom_node()).computed_properties()->for_each_property([&](auto property_id, auto& value) {
            properties.append({ CSS::string_from_property_id(property_id), value.to_string(CSS::CSSStyleValue::SerializationMode::Normal) });
        });
        quick_sort(properties, [](auto& a, auto& b) { return a.name < b.name; });

        for (auto& property : properties) {
            for (size_t i = 0; i < indent; ++i)
                builder.append("    "sv);
            builder.appendff("  ({}: {})\n", property.name, property.value);
        }
    }

    ++indent;
    layout_node.for_each_child([&](auto& child) {
        dump_tree(builder, child, show_box_model, show_cascaded_properties, interactive);
        return IterationDecision::Continue;
    });
    --indent;
}

void dump_selector(CSS::Selector const& selector)
{
    StringBuilder builder;
    dump_selector(builder, selector);
    dbgln("{}", builder.string_view());
}

static void dump_qualified_name(StringBuilder& builder, CSS::Selector::SimpleSelector::QualifiedName const& qualified_name)
{
    StringView namespace_type;
    switch (qualified_name.namespace_type) {
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Default:
        namespace_type = "Default"sv;
        break;
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::None:
        namespace_type = "None"sv;
        break;
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Any:
        namespace_type = "Any"sv;
        break;
    case CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named:
        namespace_type = "Named"sv;
        break;
    }
    builder.appendff("NamespaceType={}, Namespace='{}', Name='{}'", namespace_type, qualified_name.namespace_, qualified_name.name.name);
}

void dump_selector(StringBuilder& builder, CSS::Selector const& selector, int indent_levels)
{
    indent(builder, indent_levels);
    builder.append("CSS::Selector:\n"sv);

    for (auto& relative_selector : selector.compound_selectors()) {
        indent(builder, indent_levels + 1);

        char const* relation_description = "";
        switch (relative_selector.combinator) {
        case CSS::Selector::Combinator::None:
            relation_description = "None";
            break;
        case CSS::Selector::Combinator::ImmediateChild:
            relation_description = "ImmediateChild";
            break;
        case CSS::Selector::Combinator::Descendant:
            relation_description = "Descendant";
            break;
        case CSS::Selector::Combinator::NextSibling:
            relation_description = "AdjacentSibling";
            break;
        case CSS::Selector::Combinator::SubsequentSibling:
            relation_description = "GeneralSibling";
            break;
        case CSS::Selector::Combinator::Column:
            relation_description = "Column";
            break;
        }

        if (*relation_description)
            builder.appendff("{{{}}} ", relation_description);

        for (size_t i = 0; i < relative_selector.simple_selectors.size(); ++i) {
            auto& simple_selector = relative_selector.simple_selectors[i];
            char const* type_description = "Unknown";
            switch (simple_selector.type) {
            case CSS::Selector::SimpleSelector::Type::Universal:
                type_description = "Universal";
                break;
            case CSS::Selector::SimpleSelector::Type::Id:
                type_description = "Id";
                break;
            case CSS::Selector::SimpleSelector::Type::Class:
                type_description = "Class";
                break;
            case CSS::Selector::SimpleSelector::Type::TagName:
                type_description = "TagName";
                break;
            case CSS::Selector::SimpleSelector::Type::Attribute:
                type_description = "Attribute";
                break;
            case CSS::Selector::SimpleSelector::Type::PseudoClass:
                type_description = "PseudoClassSelector";
                break;
            case CSS::Selector::SimpleSelector::Type::PseudoElement:
                type_description = "PseudoElement";
                break;
            case CSS::Selector::SimpleSelector::Type::Nesting:
                type_description = "Nesting";
                break;
            case CSS::Selector::SimpleSelector::Type::Invalid:
                type_description = "INVALID";
                break;
            }

            builder.appendff("{}:", type_description);

            // FIXME: This is goofy
            if (simple_selector.value.has<CSS::Selector::SimpleSelector::Name>()) {
                builder.append(simple_selector.name());
            } else if (simple_selector.value.has<CSS::Selector::SimpleSelector::QualifiedName>()) {
                dump_qualified_name(builder, simple_selector.qualified_name());
            }

            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoClass) {
                auto const& pseudo_class = simple_selector.pseudo_class();

                builder.appendff(" pseudo_class={}", CSS::pseudo_class_name(pseudo_class.type));
                auto pseudo_class_metadata = CSS::pseudo_class_metadata(pseudo_class.type);

                switch (pseudo_class_metadata.parameter_type) {
                case CSS::PseudoClassMetadata::ParameterType::None:
                    break;
                case CSS::PseudoClassMetadata::ParameterType::ANPlusB:
                case CSS::PseudoClassMetadata::ParameterType::ANPlusBOf: {
                    builder.appendff("(step={}, offset={}", pseudo_class.nth_child_pattern.step_size, pseudo_class.nth_child_pattern.offset);
                    if (!pseudo_class.argument_selector_list.is_empty()) {
                        builder.append(", selectors=[\n"sv);
                        for (auto const& child_selector : pseudo_class.argument_selector_list)
                            dump_selector(builder, child_selector, indent_levels + 2);
                        indent(builder, indent_levels + 1);
                        builder.append("]"sv);
                    }
                    builder.append(")"sv);
                    break;
                }
                case CSS::PseudoClassMetadata::ParameterType::CompoundSelector:
                case CSS::PseudoClassMetadata::ParameterType::ForgivingSelectorList:
                case CSS::PseudoClassMetadata::ParameterType::ForgivingRelativeSelectorList:
                case CSS::PseudoClassMetadata::ParameterType::RelativeSelectorList:
                case CSS::PseudoClassMetadata::ParameterType::SelectorList: {
                    builder.append("([\n"sv);
                    for (auto& child_selector : pseudo_class.argument_selector_list)
                        dump_selector(builder, child_selector, indent_levels + 2);
                    indent(builder, indent_levels + 1);
                    builder.append("])"sv);
                    break;
                }
                case CSS::PseudoClassMetadata::ParameterType::Ident:
                    builder.appendff("(keyword={})", string_from_keyword(pseudo_class.keyword.value()));
                    break;
                case CSS::PseudoClassMetadata::ParameterType::LanguageRanges: {
                    builder.append('(');
                    builder.join(',', pseudo_class.languages);
                    builder.append(')');
                    break;
                }
                }
            }

            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoElement) {
                auto const& pseudo_element = simple_selector.pseudo_element();
                builder.appendff(" pseudo_element={}", CSS::pseudo_element_name(pseudo_element.type()));
                auto pseudo_element_metadata = CSS::pseudo_element_metadata(pseudo_element.type());

                switch (pseudo_element_metadata.parameter_type) {
                case CSS::PseudoElementMetadata::ParameterType::None:
                    break;
                case CSS::PseudoElementMetadata::ParameterType::PTNameSelector: {
                    auto const& [is_universal, value] = pseudo_element.pt_name_selector();
                    builder.appendff("(is_universal={}, value='{}')", is_universal, value);
                    break;
                }
                }
            }

            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Attribute) {
                auto const& attribute = simple_selector.attribute();
                char const* attribute_match_type_description = "";

                switch (attribute.match_type) {
                case CSS::Selector::SimpleSelector::Attribute::MatchType::HasAttribute:
                    attribute_match_type_description = "HasAttribute";
                    break;
                case CSS::Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch:
                    attribute_match_type_description = "ExactValueMatch";
                    break;
                case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsWord:
                    attribute_match_type_description = "ContainsWord";
                    break;
                case CSS::Selector::SimpleSelector::Attribute::MatchType::ContainsString:
                    attribute_match_type_description = "ContainsString";
                    break;
                case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment:
                    attribute_match_type_description = "StartsWithSegment";
                    break;
                case CSS::Selector::SimpleSelector::Attribute::MatchType::StartsWithString:
                    attribute_match_type_description = "StartsWithString";
                    break;
                case CSS::Selector::SimpleSelector::Attribute::MatchType::EndsWithString:
                    attribute_match_type_description = "EndsWithString";
                    break;
                }

                builder.appendff(" [{}, ", attribute_match_type_description);
                dump_qualified_name(builder, attribute.qualified_name);
                builder.appendff(", value='{}']", attribute.value);
            }

            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Invalid) {
                auto invalid = simple_selector.value.get<CSS::Selector::SimpleSelector::Invalid>();
                builder.append(" '"sv);
                for (auto const& component_value : invalid.component_values)
                    builder.append(component_value.to_string());
                builder.append("'"sv);
            }

            if (i != relative_selector.simple_selectors.size() - 1)
                builder.append(", "sv);
        }
        builder.append("\n"sv);
    }
}

void dump_rule(CSS::CSSRule const& rule)
{
    StringBuilder builder;
    dump_rule(builder, rule);
    dbgln("{}", builder.string_view());
}

void dump_rule(StringBuilder& builder, CSS::CSSRule const& rule, int indent_levels)
{
    indent(builder, indent_levels);
    builder.appendff("{}:\n", rule.class_name());

    switch (rule.type()) {
    case CSS::CSSRule::Type::FontFace:
        dump_font_face_rule(builder, as<CSS::CSSFontFaceRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Import:
        dump_import_rule(builder, as<CSS::CSSImportRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Keyframe:
    case CSS::CSSRule::Type::Keyframes:
        // TODO: Dump them!
        break;
    case CSS::CSSRule::Type::LayerBlock:
        dump_layer_block_rule(builder, as<CSS::CSSLayerBlockRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::LayerStatement:
        dump_layer_statement_rule(builder, as<CSS::CSSLayerStatementRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Media:
        dump_media_rule(builder, as<CSS::CSSMediaRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Namespace:
        dump_namespace_rule(builder, as<CSS::CSSNamespaceRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::NestedDeclarations:
        dump_nested_declarations(builder, as<CSS::CSSNestedDeclarations const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Style:
        dump_style_rule(builder, as<CSS::CSSStyleRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Supports:
        dump_supports_rule(builder, as<CSS::CSSSupportsRule const>(rule), indent_levels);
        break;
    case CSS::CSSRule::Type::Property:
        dump_property_rule(builder, as<CSS::CSSPropertyRule const>(rule), indent_levels);
        break;
    }
}

void dump_font_face_rule(StringBuilder& builder, CSS::CSSFontFaceRule const& rule, int indent_levels)
{
    auto& font_face = rule.font_face();
    indent(builder, indent_levels + 1);
    builder.appendff("font-family: {}\n", font_face.font_family());

    if (font_face.weight().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("weight: {}\n", font_face.weight().value());
    }

    if (font_face.slope().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("slope: {}\n", font_face.slope().value());
    }

    if (font_face.width().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("width: {}\n", font_face.width().value());
    }

    indent(builder, indent_levels + 1);
    builder.append("sources:\n"sv);
    for (auto const& source : font_face.sources()) {
        indent(builder, indent_levels + 2);
        if (source.local_or_url.has<URL::URL>())
            builder.appendff("url={}, format={}\n", source.local_or_url.get<URL::URL>(), source.format.value_or("???"_string));
        else
            builder.appendff("local={}\n", source.local_or_url.get<FlyString>());
    }

    indent(builder, indent_levels + 1);
    builder.append("unicode-ranges:\n"sv);
    for (auto const& unicode_range : font_face.unicode_ranges()) {
        indent(builder, indent_levels + 2);
        builder.appendff("{}\n", unicode_range.to_string());
    }

    if (font_face.ascent_override().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("ascent-override: {}\n", font_face.ascent_override().value());
    }
    if (font_face.descent_override().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("descent-override: {}\n", font_face.descent_override().value());
    }
    if (font_face.line_gap_override().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("line-gap-override: {}\n", font_face.line_gap_override().value());
    }

    indent(builder, indent_levels + 1);
    builder.appendff("display: {}\n", CSS::to_string(font_face.font_display()));

    if (font_face.font_named_instance().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("named-instance: {}\n", font_face.font_named_instance().value());
    }

    if (font_face.font_language_override().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("language-override: {}\n", font_face.font_language_override().value());
    }

    if (font_face.font_feature_settings().has_value()) {
        indent(builder, indent_levels + 1);
        builder.append("feature-settings:"sv);
        auto const& entries = font_face.font_feature_settings().value();
        for (auto const& [name, value] : entries) {
            builder.appendff(" {}={}", name, value);
        }
        builder.append("\n"sv);
    }

    if (font_face.font_variation_settings().has_value()) {
        indent(builder, indent_levels + 1);
        builder.append("variation-settings:"sv);
        auto const& entries = font_face.font_variation_settings().value();
        for (auto const& [name, value] : entries) {
            builder.appendff(" {}={}", name, value);
        }
        builder.append("\n"sv);
    }
}

void dump_import_rule(StringBuilder& builder, CSS::CSSImportRule const& rule, int indent_levels)
{
    indent(builder, indent_levels);
    builder.appendff("  Document URL: {}\n", rule.url());
}

void dump_layer_block_rule(StringBuilder& builder, CSS::CSSLayerBlockRule const& layer_block, int indent_levels)
{
    indent(builder, indent_levels);
    builder.appendff("  Layer Block: `{}`\n", layer_block.internal_name());
    indent(builder, indent_levels);
    builder.appendff("  Rules ({}):\n", layer_block.css_rules().length());
    for (auto& rule : layer_block.css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

void dump_layer_statement_rule(StringBuilder& builder, CSS::CSSLayerStatementRule const& layer_statement, int indent_levels)
{
    indent(builder, indent_levels);
    builder.append("  Layer Statement: "sv);
    builder.join(", "sv, layer_statement.name_list());
}

void dump_media_rule(StringBuilder& builder, CSS::CSSMediaRule const& media, int indent_levels)
{
    indent(builder, indent_levels);
    builder.appendff("  Media: {}\n", media.condition_text());
    indent(builder, indent_levels);
    builder.appendff("  Rules ({}):\n", media.css_rules().length());

    for (auto& rule : media.css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

void dump_supports_rule(StringBuilder& builder, CSS::CSSSupportsRule const& supports, int indent_levels)
{
    indent(builder, indent_levels);
    builder.append("  Supports:\n"sv);
    supports.supports().dump(builder, indent_levels + 2);
    indent(builder, indent_levels);
    builder.appendff("  Rules ({}):\n", supports.css_rules().length());

    for (auto& rule : supports.css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

void dump_property_rule(StringBuilder& builder, CSS::CSSPropertyRule const& property, int indent_levels)
{
    indent(builder, indent_levels + 1);
    builder.appendff("name: {}\n", property.name());

    indent(builder, indent_levels + 1);
    builder.appendff("syntax: {}\n", property.syntax());

    indent(builder, indent_levels + 1);
    builder.appendff("inherits: {}\n", property.inherits());

    if (property.initial_value().has_value()) {
        indent(builder, indent_levels + 1);
        builder.appendff("initial-value: {}\n", property.initial_value().value());
    }
}

void dump_style_properties(StringBuilder& builder, CSS::CSSStyleProperties const& declaration, int indent_levels)
{
    indent(builder, indent_levels);
    builder.appendff("Declarations ({}):\n", declaration.length());
    for (auto& property : declaration.properties()) {
        indent(builder, indent_levels);
        builder.appendff("  {}: '{}'", CSS::string_from_property_id(property.property_id), property.value->to_string(CSS::CSSStyleValue::SerializationMode::Normal));
        if (property.important == CSS::Important::Yes)
            builder.append(" \033[31;1m!important\033[0m"sv);
        builder.append('\n');
    }
    for (auto& property : declaration.custom_properties()) {
        indent(builder, indent_levels);
        builder.appendff("  {}: '{}'", property.key, property.value.value->to_string(CSS::CSSStyleValue::SerializationMode::Normal));
        if (property.value.important == CSS::Important::Yes)
            builder.append(" \033[31;1m!important\033[0m"sv);
        builder.append('\n');
    }
}

void dump_style_rule(StringBuilder& builder, CSS::CSSStyleRule const& rule, int indent_levels)
{
    for (auto& selector : rule.selectors()) {
        dump_selector(builder, selector, indent_levels + 1);
    }
    dump_style_properties(builder, rule.declaration(), indent_levels + 1);

    indent(builder, indent_levels);
    builder.appendff("  Child rules ({}):\n", rule.css_rules().length());
    for (auto& child_rule : rule.css_rules())
        dump_rule(builder, child_rule, indent_levels + 2);
}

void dump_sheet(CSS::StyleSheet const& sheet)
{
    StringBuilder builder;
    dump_sheet(builder, sheet);
    dbgln("{}", builder.string_view());
}

void dump_sheet(StringBuilder& builder, CSS::StyleSheet const& sheet)
{
    auto& css_stylesheet = as<CSS::CSSStyleSheet>(sheet);

    builder.appendff("CSSStyleSheet{{{}}}: {} rule(s)\n", &sheet, css_stylesheet.rules().length());

    for (auto& rule : css_stylesheet.rules())
        dump_rule(builder, rule);
}

void dump_tree(Painting::Paintable const& paintable)
{
    StringBuilder builder;
    dump_tree(builder, paintable, true);
    dbgln("{}", builder.string_view());
}

void dump_tree(StringBuilder& builder, Painting::Paintable const& paintable, bool colorize, int indent)
{
    for (int i = 0; i < indent; ++i)
        builder.append("  "sv);

    StringView paintable_with_lines_color_on = ""sv;
    StringView paintable_box_color_on = ""sv;
    StringView text_paintable_color_on = ""sv;
    StringView paintable_color_on = ""sv;
    StringView color_off = ""sv;

    if (colorize) {
        paintable_with_lines_color_on = "\033[34m"sv;
        paintable_box_color_on = "\033[33m"sv;
        text_paintable_color_on = "\033[35m"sv;
        paintable_color_on = "\033[32m"sv;
        color_off = "\033[0m"sv;
    }

    if (is<Painting::PaintableWithLines>(paintable))
        builder.append(paintable_with_lines_color_on);
    else if (is<Painting::PaintableBox>(paintable))
        builder.append(paintable_box_color_on);
    else if (is<Painting::TextPaintable>(paintable))
        builder.append(text_paintable_color_on);
    else
        builder.append(paintable_color_on);

    builder.appendff("{}{} ({})", paintable.class_name(), color_off, paintable.layout_node().debug_description());

    if (paintable.layout_node().is_box()) {
        auto const& paintable_box = static_cast<Painting::PaintableBox const&>(paintable);
        builder.appendff(" {}", paintable_box.absolute_border_box_rect());

        if (paintable_box.has_scrollable_overflow()) {
            builder.appendff(" overflow: {}", paintable_box.scrollable_overflow_rect());
        }

        if (!paintable_box.scroll_offset().is_zero()) {
            builder.appendff(" scroll-offset: {}", paintable_box.scroll_offset());
        }
    }
    builder.append("\n"sv);
    for (auto const* child = paintable.first_child(); child; child = child->next_sibling()) {
        dump_tree(builder, *child, colorize, indent + 1);
    }
}

void dump_namespace_rule(StringBuilder& builder, CSS::CSSNamespaceRule const& namespace_, int indent_levels)
{
    indent(builder, indent_levels);
    builder.appendff("  Namespace: {}\n", namespace_.namespace_uri());
    if (!namespace_.prefix().is_empty())
        builder.appendff("  Prefix: {}\n", namespace_.prefix());
}

void dump_nested_declarations(StringBuilder& builder, CSS::CSSNestedDeclarations const& declarations, int indent_levels)
{
    indent(builder, indent_levels);
    builder.append("  Nested declarations:\n"sv);
    dump_style_properties(builder, declarations.declaration(), indent_levels + 1);
}
}
