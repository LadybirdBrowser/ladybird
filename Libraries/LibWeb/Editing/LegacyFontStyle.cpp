/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/LegacyFontStyle.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFontElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Namespace.h>

namespace Web::Editing {

static GC::Ptr<HTML::HTMLFontElement> single_legacy_font_run(DOM::Node& node)
{
    if (node.child_count() != 1)
        return nullptr;

    return as_if<HTML::HTMLFontElement>(node.last_child());
}

void remove_single_run_legacy_font_style(DOM::Node& node)
{
    auto font = single_legacy_font_run(node);
    if (!font)
        return;
    unwrap_node_preserving_ranges(*font);
}

enum class ChildRunMaterialization {
    PreserveTextNodeSeams,
    PreserveAllChildRuns,
};

static void materialize_single_run_legacy_font_style(DOM::Node& node, HTML::HTMLFontElement& font, Utf16String materialized_color, ChildRunMaterialization child_run_materialization)
{
    // INTEROP: Blink and WebKit traverse a rendered range when serializing editing content. If that traversal contains
    //          one presentational wrapper, the wrapper is omitted and supplies CSS to the serialized content. A range
    //          which crosses multiple inline runs includes their elements directly, so their legacy markup remains.
    if (!font.has_children()) {
        remove_node(font);
        return;
    }

    // INTEROP: Serialized paragraph moves protect trailing collapsible whitespace as a separate text run. Blink and
    //          WebKit materialize each resulting run in its own style span, and later whitespace cleanup decides
    //          whether the protected space becomes ordinary without erasing that observable node seam.
    bool preserve_child_runs = font.child_count() > 1;
    if (child_run_materialization == ChildRunMaterialization::PreserveTextNodeSeams) {
        for (auto* child = font.first_child(); child && preserve_child_runs; child = child->next_sibling())
            preserve_child_runs = is<DOM::Text>(*child);
    }

    if (auto* text = as_if<DOM::Text>(font.first_child()); text && !text->next_sibling()) {
        auto data = text->data();
        size_t trailing_whitespace_start = data.length_in_code_units();
        while (trailing_whitespace_start > 0) {
            auto code_unit = data.code_unit_at(trailing_whitespace_start - 1);
            if (!is_ascii_space(code_unit) && code_unit != 0xa0)
                break;
            --trailing_whitespace_start;
        }
        if (trailing_whitespace_start > 0 && trailing_whitespace_start < data.length_in_code_units()) {
            MUST(split_text(*text, trailing_whitespace_start));
            preserve_child_runs = true;
        }
    }

    do {
        auto span_element = MUST(DOM::create_element(node.document(), HTML::TagNames::span, Namespace::HTML));
        auto& span = static_cast<HTML::HTMLElement&>(*span_element);
        {
            // NB: The span is detached staging content. Its eventual insertion is recorded as a whole, so construction
            //     of its inline style must not be diagnosed as an unrecorded mutation of the edited document.
            EditingHistory::ProxyMutationScope mutation_scope { span };
            MUST(span.style_for_bindings()->set_property(CSS::PropertyID::Color, materialized_color));
        }
        insert_node_before(span, node, font);
        move_node_preserving_ranges(*font.first_child(), span, 0);
        if (!preserve_child_runs) {
            while (font.first_child())
                move_node_preserving_ranges(*font.first_child(), span, span.child_count());
        }
    } while (font.first_child());
    remove_node(font);
}

void materialize_single_run_legacy_font_style(DOM::Node& node)
{
    auto font = single_legacy_font_run(node);
    if (!font)
        return;
    auto color_attribute = font->get_attribute(HTML::AttributeNames::color);
    if (!color_attribute.has_value())
        return;

    auto materialized_color = *color_attribute;
    if (font->document().in_quirks_mode()) {
        // INTEROP: In quirks mode Blink and WebKit materialize the computed HTML presentation hint. Standards mode
        //          instead feeds the authored attribute token through CSS, where values such as #ff00 have a different
        //          meaning. Preserve that document-mode distinction when replacing legacy markup with inline style.
        auto legacy_color = HTML::parse_legacy_color_value(*color_attribute);
        if (!legacy_color.has_value())
            return;
        auto css_value = parse_css_value(CSS::Parser::ParsingParams { font->document() }, color_attribute->utf16_view(), CSS::PropertyID::Color);
        if (!css_value || css_value->to_color({}) != legacy_color) {
            auto legacy_color_value = CSS::ColorStyleValue::create_from_color(*legacy_color, CSS::ColorSyntax::Legacy);
            materialized_color = Utf16String::from_utf8(legacy_color_value->to_string(CSS::SerializationMode::Normal));
        }
    }

    materialize_single_run_legacy_font_style(node, *font, move(materialized_color), ChildRunMaterialization::PreserveTextNodeSeams);
}

void materialize_single_run_legacy_font_style(DOM::Node& node, DOM::Node& rendered_style_source)
{
    auto font = single_legacy_font_run(node);
    if (!font)
        return;
    auto color = resolved_value(rendered_style_source, CSS::PropertyID::Color);
    if (!color)
        return;

    // The serializer has already isolated protected whitespace in child wrappers. Materializing each child as its own
    // styled run lets fragment cleanup discard those transport wrappers without erasing the inherited style seam.
    materialize_single_run_legacy_font_style(node, *font,
        Utf16String::from_utf8(color->to_string(CSS::SerializationMode::Normal)), ChildRunMaterialization::PreserveAllChildRuns);
}

}
