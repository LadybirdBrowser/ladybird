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
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/SVGBox.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web {

static void dump_session_history_entry(StringBuilder& builder, HTML::SessionHistoryEntry const& session_history_entry, int indent_levels)
{
    dump_indent(builder, indent_levels);
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
    if (auto const* element = as_if<DOM::Element>(node)) {
        auto namespace_prefix = [&] -> FlyString {
            auto const& namespace_uri = element->namespace_uri();
            if (!namespace_uri.has_value() || node.document().is_default_namespace(namespace_uri.value().to_string()))
                return ""_fly_string;
            if (namespace_uri == Namespace::HTML)
                return "html:"_fly_string;
            if (namespace_uri == Namespace::SVG)
                return "svg:"_fly_string;
            if (namespace_uri == Namespace::MathML)
                return "mathml:"_fly_string;
            return *namespace_uri;
        }();

        builder.appendff("<{}{}", namespace_prefix, element->local_name());
        element->for_each_attribute([&](auto& name, auto& value) {
            builder.appendff(" {}={}", name, value);
        });
        builder.append(">\n"sv);
        if (element->use_pseudo_element().has_value()) {
            for (int i = 0; i < indent; ++i)
                builder.append("  "sv);
            builder.appendff("  (pseudo-element: {})\n", CSS::pseudo_element_name(element->use_pseudo_element().value()));
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

void dump_tree(Layout::Node const& layout_node, bool show_cascaded_properties)
{
    StringBuilder builder;
    dump_tree(builder, layout_node, show_cascaded_properties, true);
    dbgln("{}", builder.string_view());
}

void dump_tree(StringBuilder& builder, Layout::Node const& layout_node, bool show_cascaded_properties, bool interactive)
{
    static size_t indent = 0;
    builder.append_repeated("  "sv, indent);

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
        StringBuilder identifier_builder;
        if (element.id().has_value() && !element.id()->is_empty()) {
            identifier_builder.append('#');
            identifier_builder.append(*element.id());
        }
        for (auto& class_name : element.class_names()) {
            identifier_builder.append('.');
            identifier_builder.append(class_name);
        }
        identifier = MUST(identifier_builder.to_string());
    }

    StringView nonbox_color_on = ""sv;
    StringView box_color_on = ""sv;
    StringView svg_box_color_on = ""sv;
    StringView positioned_color_on = ""sv;
    StringView floating_color_on = ""sv;
    StringView inline_color_on = ""sv;
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
        fragment_color_on = "\033[35;1m"sv;
        flex_color_on = "\033[34;1m"sv;
        table_color_on = "\033[91;1m"sv;
        formatting_context_color_on = "\033[37;1m"sv;
        color_off = "\033[0m"sv;
    }

    auto dump_position = [&] {
        if (auto* paintable_box = as_if<Painting::PaintableBox>(layout_node.first_paintable()))
            builder.appendff("at {}", paintable_box->absolute_rect().location());
        else
            builder.appendff("(not painted)");
    };
    auto dump_box_model = [&] {
        if (auto const* paintable_box = as_if<Painting::PaintableBox>(layout_node.first_paintable())) {
            auto const& box_model = paintable_box->box_model();
            // Dump the horizontal box properties
            builder.appendff(" [{}+{}+{} {} {}+{}+{}]",
                box_model.margin.left,
                box_model.border.left,
                box_model.padding.left,
                paintable_box->content_width(),
                box_model.padding.right,
                box_model.border.right,
                box_model.margin.right);

            // And the vertical box properties
            builder.appendff(" [{}+{}+{} {} {}+{}+{}]",
                box_model.margin.top,
                box_model.border.top,
                box_model.padding.top,
                paintable_box->content_height(),
                box_model.padding.bottom,
                box_model.border.bottom,
                box_model.margin.bottom);
        }
    };

    if (!is<Layout::Box>(layout_node)) {
        builder.appendff("{}{}{} <{}{}{}{}> ",
            nonbox_color_on,
            layout_node.class_name(),
            color_off,
            tag_name,
            nonbox_color_on,
            identifier,
            color_off);

        dump_position();
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

        dump_position();

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
            case Layout::FormattingContext::Type::Flex:
            case Layout::FormattingContext::Type::Grid:
            case Layout::FormattingContext::Type::Table:
            case Layout::FormattingContext::Type::SVG:
                builder.appendff(" [{}{}{}]", formatting_context_color_on, Layout::FormattingContext::type_name(formatting_context_type.value()), color_off);
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
                    dump_tree(builder, *nested_layout_root, show_cascaded_properties, interactive);
                    --indent;
                }
            }
        }
    }

    if (auto const* potential_continuation_node = as_if<Layout::NodeWithStyleAndBoxModelMetrics>(layout_node);
        potential_continuation_node && potential_continuation_node->continuation_of_node())
        builder.append(" continuation"sv);

    builder.append("\n"sv);

    if (layout_node.dom_node() && is<HTML::HTMLImageElement>(*layout_node.dom_node())) {
        if (auto image_data = static_cast<HTML::HTMLImageElement const&>(*layout_node.dom_node()).current_request().image_data()) {
            if (is<SVG::SVGDecodedImageData>(*image_data)) {
                auto& svg_data = as<SVG::SVGDecodedImageData>(*image_data);
                // NB: Called from debug dump code, no layout guarantee.
                if (svg_data.svg_document().unsafe_layout_node()) {
                    ++indent;
                    for (size_t i = 0; i < indent; ++i)
                        builder.append("  "sv);
                    builder.append("(SVG-as-image isolated context)\n"sv);

                    dump_tree(builder, *svg_data.svg_document().unsafe_layout_node(), show_cascaded_properties, interactive);
                    --indent;
                }
            }
        }
    }

    auto dump_fragment = [&](auto& fragment, size_t fragment_index) {
        builder.append_repeated("  "sv, indent);
        builder.appendff("  {}frag {}{} from {} ",
            fragment_color_on,
            fragment_index,
            color_off,
            fragment.layout_node().class_name());
        builder.appendff("start: {}, length: {}, rect: {} baseline: {}\n",
            fragment.start_offset(),
            fragment.length_in_code_units(),
            fragment.absolute_rect(),
            fragment.baseline());
        if (fragment.length_in_code_units() > 0) {
            builder.append_repeated("  "sv, indent);
            builder.appendff("      \"{}\"\n", fragment.text());
        }
    };

    if (auto const* block_container = as_if<Layout::BlockContainer>(layout_node);
        block_container && block_container->children_are_inline() && block_container->paintable_with_lines()) {
        size_t fragment_index = 0;
        for (auto const& fragment : block_container->paintable_with_lines()->fragments())
            dump_fragment(fragment, fragment_index++);
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
            properties.append({ CSS::string_from_property_id(property_id), value.to_string(CSS::SerializationMode::Normal) });
        });
        quick_sort(properties, [](auto& a, auto& b) { return a.name < b.name; });

        for (auto& property : properties) {
            builder.append_repeated("    "sv, indent);
            builder.appendff("  ({}: {})\n", property.name, property.value);
        }
    }

    ++indent;
    layout_node.for_each_child([&](auto& child) {
        dump_tree(builder, child, show_cascaded_properties, interactive);
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
    dump_indent(builder, indent_levels);
    builder.append("CSS::Selector:\n"sv);

    for (auto& relative_selector : selector.compound_selectors()) {
        dump_indent(builder, indent_levels + 1);

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
                    builder.appendff("(step={}, offset={}", pseudo_class.an_plus_b_pattern.step_size, pseudo_class.an_plus_b_pattern.offset);
                    if (!pseudo_class.argument_selector_list.is_empty()) {
                        builder.append(", selectors=[\n"sv);
                        for (auto const& child_selector : pseudo_class.argument_selector_list)
                            dump_selector(builder, child_selector, indent_levels + 2);
                        dump_indent(builder, indent_levels + 1);
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
                    dump_indent(builder, indent_levels + 1);
                    builder.append("])"sv);
                    break;
                }
                case CSS::PseudoClassMetadata::ParameterType::Ident:
                    builder.appendff("(ident={})", pseudo_class.ident->string_value);
                    break;
                case CSS::PseudoClassMetadata::ParameterType::LanguageRanges: {
                    builder.append('(');
                    builder.join(',', pseudo_class.languages);
                    builder.append(')');
                    break;
                }
                case CSS::PseudoClassMetadata::ParameterType::LevelList: {
                    builder.append('(');
                    builder.join(',', pseudo_class.levels);
                    builder.append(')');
                    break;
                }
                }
            }

            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoElement) {
                auto const& pseudo_element = simple_selector.pseudo_element();
                builder.appendff(" pseudo_element={}", pseudo_element.serialize());
                auto pseudo_element_metadata = CSS::pseudo_element_metadata(pseudo_element.type());

                switch (pseudo_element_metadata.parameter_type) {
                case CSS::PseudoElementMetadata::ParameterType::None:
                case CSS::PseudoElementMetadata::ParameterType::CompoundSelector:
                case CSS::PseudoElementMetadata::ParameterType::IdentList:
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
        builder.appendff("  {}: '{}'", CSS::to_string(descriptor.descriptor_id), descriptor.value->to_string(CSS::SerializationMode::Normal));
        builder.append('\n');
    }
}

void dump_sheet(CSS::StyleSheet const& sheet)
{
    StringBuilder builder;
    dump_sheet(builder, sheet);
    dbgln("{}", builder.string_view());
}

void dump_sheet(StringBuilder& builder, CSS::StyleSheet const& sheet, int indent_levels)
{
    dump_indent(builder, indent_levels);
    auto& css_stylesheet = as<CSS::CSSStyleSheet>(sheet);
    builder.appendff("CSSStyleSheet{{{}}}: {} rule(s)\n", &sheet, css_stylesheet.rules().length());

    for (auto& rule : css_stylesheet.rules())
        dump_rule(builder, rule, indent_levels + 1);
}

void dump_tree(Painting::Paintable const& paintable)
{
    StringBuilder builder;
    dump_tree(builder, paintable, true);
    dbgln("{}", builder.string_view());
}

void dump_tree(StringBuilder& builder, Painting::Paintable const& paintable, bool colorize, int indent)
{
    // Dump all paintables attached to this layout node at every level.
    // This makes detached/disconnected paintables visible across the full subtree.
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

    bool dumped_any = false;
    for (auto const& node_paintable : paintable.layout_node().paintables()) {
        if (dumped_any)
            builder.append("\n"sv);

        for (int i = 0; i < indent; ++i)
            builder.append("  "sv);

        if (is<Painting::PaintableWithLines>(node_paintable))
            builder.append(paintable_with_lines_color_on);
        else if (is<Painting::PaintableBox>(node_paintable))
            builder.append(paintable_box_color_on);
        else if (is<Painting::TextPaintable>(node_paintable))
            builder.append(text_paintable_color_on);
        else
            builder.append(paintable_color_on);

        builder.appendff("{}{} ({})", node_paintable.class_name(), color_off, node_paintable.layout_node().debug_description());

        if (auto const* paintable_box = as_if<Painting::PaintableBox>(node_paintable)) {
            builder.appendff(" {}", paintable_box->absolute_border_box_rect());

            if (paintable_box->has_scrollable_overflow())
                builder.appendff(" overflow: {}", paintable_box->scrollable_overflow_rect());

            if (!paintable_box->scroll_offset().is_zero())
                builder.appendff(" scroll-offset: {}", paintable_box->scroll_offset());
        }
        builder.append("\n"sv);

        for (auto const* child = node_paintable.first_child(); child; child = child->next_sibling())
            dump_tree(builder, *child, colorize, indent + 1);
        dumped_any = true;
    }
}

}
