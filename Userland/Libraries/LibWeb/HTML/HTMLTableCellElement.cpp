/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableCellElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLTableCellElement);

HTMLTableCellElement::HTMLTableCellElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableCellElement::~HTMLTableCellElement() = default;

void HTMLTableCellElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableCellElement);
}

void HTMLTableCellElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::bgcolor) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                style.set_property(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
            return;
        }
        if (name == HTML::AttributeNames::valign) {
            if (auto parsed_value = parse_css_value(CSS::Parser::ParsingContext { document() }, value, CSS::PropertyID::VerticalAlign))
                style.set_property(CSS::PropertyID::VerticalAlign, parsed_value.release_nonnull());
            return;
        }
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("center"sv) || value.equals_ignoring_ascii_case("middle"sv)) {
                style.set_property(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebCenter));
            } else if (value.equals_ignoring_ascii_case("left"sv)) {
                style.set_property(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebLeft));
            } else if (value.equals_ignoring_ascii_case("right"sv)) {
                style.set_property(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebRight));
            } else {
                if (auto parsed_value = parse_css_value(CSS::Parser::ParsingContext { document() }, value, CSS::PropertyID::TextAlign))
                    style.set_property(CSS::PropertyID::TextAlign, parsed_value.release_nonnull());
            }
            return;
        }
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_nonzero_dimension_value(value))
                style.set_property(CSS::PropertyID::Width, parsed_value.release_nonnull());
            return;
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_nonzero_dimension_value(value))
                style.set_property(CSS::PropertyID::Height, parsed_value.release_nonnull());
            return;
        } else if (name == HTML::AttributeNames::background) {
            if (auto parsed_value = document().parse_url(value); parsed_value.is_valid())
                style.set_property(CSS::PropertyID::BackgroundImage, CSS::ImageStyleValue::create(parsed_value));
            return;
        }
    });

    auto const table_element = first_ancestor_of_type<HTMLTableElement>();
    if (!table_element)
        return;

    if (auto padding = table_element->padding()) {
        style.set_property(CSS::PropertyID::PaddingTop, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
        style.set_property(CSS::PropertyID::PaddingBottom, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
        style.set_property(CSS::PropertyID::PaddingLeft, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
        style.set_property(CSS::PropertyID::PaddingRight, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
    }

    auto border = table_element->border();

    if (!border)
        return;
    auto apply_border_style = [&](CSS::PropertyID style_property, CSS::PropertyID width_property, CSS::PropertyID color_property) {
        style.set_property(style_property, CSS::CSSKeywordValue::create(CSS::Keyword::Inset));
        style.set_property(width_property, CSS::LengthStyleValue::create(CSS::Length::make_px(1)));
        style.set_property(color_property, table_element->computed_css_values()->property(color_property));
    };
    apply_border_style(CSS::PropertyID::BorderLeftStyle, CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor);
    apply_border_style(CSS::PropertyID::BorderTopStyle, CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor);
    apply_border_style(CSS::PropertyID::BorderRightStyle, CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor);
    apply_border_style(CSS::PropertyID::BorderBottomStyle, CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor);
}

// This implements step 8 in the spec here:
// https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
unsigned int HTMLTableCellElement::col_span() const
{
    auto optional_value = Web::HTML::parse_non_negative_integer(get_attribute_value(HTML::AttributeNames::colspan));

    // If parsing that value failed, or returned zero, or if the attribute is absent, then let colspan be 1, instead.
    if (!optional_value.has_value() || optional_value.value() == 0) {
        return 1;
    }

    auto value = optional_value.value();

    // If colspan is greater than 1000, let it be 1000 instead.
    if (value > 1000) {
        return 1000;
    }

    return value;
}

WebIDL::ExceptionOr<void> HTMLTableCellElement::set_col_span(unsigned int value)
{
    return set_attribute(HTML::AttributeNames::colspan, MUST(String::number(value)));
}

// This implements step 9 in the spec here:
// https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
unsigned int HTMLTableCellElement::row_span() const
{
    // If parsing that value failed or if the attribute is absent, then let rowspan be 1, instead.
    auto value = Web::HTML::parse_non_negative_integer(get_attribute_value(HTML::AttributeNames::rowspan)).value_or(1);

    // If rowspan is greater than 65534, let it be 65534 instead.
    if (value > 65534) {
        return 65534;
    }

    return value;
}

WebIDL::ExceptionOr<void> HTMLTableCellElement::set_row_span(unsigned int value)
{
    return set_attribute(HTML::AttributeNames::rowspan, MUST(String::number(value)));
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tdth-cellindex
WebIDL::Long HTMLTableCellElement::cell_index() const
{
    // The cellIndex IDL attribute must, if the element has a parent tr element, return the index of the cell's
    // element in the parent element's cells collection. If there is no such parent element, then the attribute
    // must return −1.
    auto const* parent = first_ancestor_of_type<HTMLTableRowElement>();
    if (!parent)
        return -1;

    auto rows = parent->cells()->collect_matching_elements();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] == this)
            return i;
    }
    return -1;
}

Optional<ARIA::Role> HTMLTableCellElement::default_role() const
{
    // TODO: For td:
    //       role=cell if the ancestor table element is exposed as a role=table
    //       role=gridcell if the ancestor table element is exposed as a role=grid or treegrid
    //       No corresponding role if the ancestor table element is not exposed as a role=table, grid or treegrid
    //       For th:
    //       role=columnheader, rowheader or cell if the ancestor table element is exposed as a role=table
    //        role=columnheader, rowheader or gridcell if the ancestor table element is exposed as a role=grid or treegrid
    //        No corresponding role if the ancestor table element is not exposed as a role=table, grid or treegrid
    // https://www.w3.org/TR/html-aria/#el-td
    // https://www.w3.org/TR/html-aria/#el-th
    return {};
}

}
