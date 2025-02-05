/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableCellElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
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

GC_DEFINE_ALLOCATOR(HTMLTableCellElement);

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

bool HTMLTableCellElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::background,
        HTML::AttributeNames::bgcolor,
        HTML::AttributeNames::height,
        HTML::AttributeNames::valign,
        HTML::AttributeNames::width);
}

void HTMLTableCellElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::bgcolor) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
            return;
        }
        if (name == HTML::AttributeNames::valign) {
            if (auto parsed_value = parse_css_value(CSS::Parser::ParsingParams { document() }, value, CSS::PropertyID::VerticalAlign))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::VerticalAlign, parsed_value.release_nonnull());
            return;
        }
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("center"sv) || value.equals_ignoring_ascii_case("middle"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebCenter));
            } else if (value.equals_ignoring_ascii_case("left"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebLeft));
            } else if (value.equals_ignoring_ascii_case("right"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebRight));
            } else {
                if (auto parsed_value = parse_css_value(CSS::Parser::ParsingParams { document() }, value, CSS::PropertyID::TextAlign))
                    cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, parsed_value.release_nonnull());
            }
            return;
        }
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_nonzero_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, parsed_value.release_nonnull());
            return;
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_nonzero_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, parsed_value.release_nonnull());
            return;
        } else if (name == HTML::AttributeNames::background) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:encoding-parsing-and-serializing-a-url
            if (auto parsed_value = document().encoding_parse_url(value); parsed_value.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundImage, CSS::ImageStyleValue::create(*parsed_value));
            return;
        }
    });

    auto const table_element = first_ancestor_of_type<HTMLTableElement>();
    if (!table_element)
        return;

    if (auto padding = table_element->padding()) {
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::PaddingTop, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::PaddingBottom, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::PaddingLeft, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::PaddingRight, CSS::LengthStyleValue::create(CSS::Length::make_px(padding)));
    }

    auto border = table_element->border();

    if (!border)
        return;
    auto apply_border_style = [&](CSS::PropertyID style_property, CSS::PropertyID width_property, CSS::PropertyID color_property) {
        cascaded_properties->set_property_from_presentational_hint(style_property, CSS::CSSKeywordValue::create(CSS::Keyword::Inset));
        cascaded_properties->set_property_from_presentational_hint(width_property, CSS::LengthStyleValue::create(CSS::Length::make_px(1)));
        cascaded_properties->set_property_from_presentational_hint(color_property, table_element->computed_properties()->property(color_property));
    };
    apply_border_style(CSS::PropertyID::BorderLeftStyle, CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor);
    apply_border_style(CSS::PropertyID::BorderTopStyle, CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor);
    apply_border_style(CSS::PropertyID::BorderRightStyle, CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor);
    apply_border_style(CSS::PropertyID::BorderBottomStyle, CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor);
}

// This implements step 8 in the spec here:
// https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
WebIDL::UnsignedLong HTMLTableCellElement::col_span() const
{
    auto col_span_attribute = get_attribute(HTML::AttributeNames::colspan);
    if (!col_span_attribute.has_value())
        return 1;

    auto optional_value_digits = Web::HTML::parse_non_negative_integer_digits(*col_span_attribute);

    // If parsing that value failed, or returned zero, or if the attribute is absent, then let colspan be 1, instead.
    if (!optional_value_digits.has_value())
        return 1;

    auto optional_value = optional_value_digits->to_number<i64>(TrimWhitespace::No);
    if (optional_value == 0)
        return 1;

    // NOTE: If there is no value at this point the value must be larger than NumericLimits<i64>::max(), so return the maximum value of 1000.
    if (!optional_value.has_value())
        return 1000;

    auto value = optional_value.value();

    // If colspan is greater than 1000, let it be 1000 instead.
    if (value > 1000) {
        return 1000;
    }

    return value;
}

WebIDL::ExceptionOr<void> HTMLTableCellElement::set_col_span(WebIDL::UnsignedLong value)
{
    if (value > 2147483647)
        value = 1;
    return set_attribute(HTML::AttributeNames::colspan, String::number(value));
}

// This implements step 9 in the spec here:
// https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
WebIDL::UnsignedLong HTMLTableCellElement::row_span() const
{
    auto row_span_attribute = get_attribute(HTML::AttributeNames::rowspan);
    if (!row_span_attribute.has_value())
        return 1;

    // If parsing that value failed or if the attribute is absent, then let rowspan be 1, instead.
    auto optional_value_digits = Web::HTML::parse_non_negative_integer_digits(*row_span_attribute);
    if (!optional_value_digits.has_value())
        return 1;

    auto optional_value = optional_value_digits->to_number<i64>(TrimWhitespace::No);

    // If rowspan is greater than 65534, let it be 65534 instead.
    // NOTE: If there is no value at this point the value must be larger than NumericLimits<i64>::max(), so return the maximum value of 65534.
    if (!optional_value.has_value() || *optional_value > 65534)
        return 65534;

    return *optional_value;
}

WebIDL::ExceptionOr<void> HTMLTableCellElement::set_row_span(WebIDL::UnsignedLong value)
{
    if (value > 2147483647)
        value = 1;
    return set_attribute(HTML::AttributeNames::rowspan, String::number(value));
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
    if (local_name() == TagNames::th) {
        for (auto const* ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
            // AD-HOC: The ancestor checks here aren’t explicitly defined in the spec, but implicitly follow from what
            // the spec does state, and from the physical placement/layout of elements. Also, the el-th and el-th-in-row
            // tests at https://wpt.fyi/results/html-aam/table-roles.html require doing these ancestor checks — and
            // implementing them causes the behavior to match that of other engines.
            // https://w3c.github.io/html-aam/#el-th-columnheader
            if (get_attribute(HTML::AttributeNames::scope) == "columnheader" || ancestor->local_name() == TagNames::thead)
                return ARIA::Role::columnheader;
            // https://w3c.github.io/html-aam/#el-th-rowheader
            if (get_attribute(HTML::AttributeNames::scope) == "rowheader" || ancestor->local_name() == TagNames::tbody)
                return ARIA::Role::rowheader;
        }
    }
    auto const* table_element = first_ancestor_of_type<HTMLTableElement>();
    // https://w3c.github.io/html-aam/#el-td
    // https://w3c.github.io/html-aam/#el-th/
    // (ancestor table element has table role)
    if (table_element->role_or_default() == ARIA::Role::table)
        return ARIA::Role::cell;
    // https://w3c.github.io/html-aam/#el-td-gridcell
    // https://w3c.github.io/html-aam/#el-th-gridcell
    // (ancestor table element has grid or treegrid role)
    if (first_is_one_of(table_element->role_or_default(), ARIA::Role::grid, ARIA::Role::gridcell))
        return ARIA::Role::gridcell;
    return {};
}

}
