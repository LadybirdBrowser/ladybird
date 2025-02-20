/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Adam Hodgen <ant1441@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTableElement);

HTMLTableElement::HTMLTableElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableElement::~HTMLTableElement() = default;

void HTMLTableElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableElement);
}

void HTMLTableElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rows);
    visitor.visit(m_t_bodies);
}

static unsigned parse_border(StringView value)
{
    return value.to_number<unsigned>().value_or(0);
}

bool HTMLTableElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::background,
        HTML::AttributeNames::bgcolor,
        HTML::AttributeNames::border,
        HTML::AttributeNames::bordercolor,
        HTML::AttributeNames::cellpadding,
        HTML::AttributeNames::cellspacing,
        HTML::AttributeNames::height,
        HTML::AttributeNames::width);
}

void HTMLTableElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_nonzero_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, parsed_value.release_nonnull());
            return;
        }
        if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, parsed_value.release_nonnull());
            return;
        }
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("center"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginLeft, CSS::CSSKeywordValue::create(CSS::Keyword::Auto));
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginRight, CSS::CSSKeywordValue::create(CSS::Keyword::Auto));
            } else if (auto parsed_value = parse_css_value(CSS::Parser::ParsingParams { document() }, value, CSS::PropertyID::Float)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Float, parsed_value.release_nonnull());
            }
            return;
        }
        if (name == HTML::AttributeNames::background) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:encoding-parsing-and-serializing-a-url
            if (auto parsed_value = document().encoding_parse_url(value); parsed_value.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundImage, CSS::ImageStyleValue::create(*parsed_value));
            return;
        }
        if (name == HTML::AttributeNames::bgcolor) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
            return;
        }
        if (name == HTML::AttributeNames::cellspacing) {
            if (auto parsed_value = parse_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderSpacing, parsed_value.release_nonnull());
            return;
        }
        if (name == HTML::AttributeNames::border) {
            auto border = parse_border(value);
            if (!border)
                return;
            auto apply_border_style = [&](CSS::PropertyID style_property, CSS::PropertyID width_property, CSS::PropertyID color_property) {
                auto legacy_line_style = CSS::CSSKeywordValue::create(CSS::Keyword::Outset);
                cascaded_properties->set_property_from_presentational_hint(style_property, legacy_line_style);
                cascaded_properties->set_property_from_presentational_hint(width_property, CSS::LengthStyleValue::create(CSS::Length::make_px(border)));
                cascaded_properties->set_property_from_presentational_hint(color_property, CSS::CSSColorValue::create_from_color(Color(128, 128, 128)));
            };
            apply_border_style(CSS::PropertyID::BorderLeftStyle, CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor);
            apply_border_style(CSS::PropertyID::BorderTopStyle, CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor);
            apply_border_style(CSS::PropertyID::BorderRightStyle, CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor);
            apply_border_style(CSS::PropertyID::BorderBottomStyle, CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor);
        }
        if (name == HTML::AttributeNames::bordercolor) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:attr-table-bordercolor
            // When a table element has a bordercolor attribute, its value is expected to be parsed using the rules for parsing a legacy color value,
            // and if that does not return failure, the user agent is expected to treat the attribute as a presentational hint setting the element's
            // 'border-top-color', 'border-right-color', 'border-bottom-color', and 'border-left-color' properties to the resulting color.
            if (auto parsed_color = parse_legacy_color_value(value); parsed_color.has_value()) {
                auto color_value = CSS::CSSColorValue::create_from_color(parsed_color.value());
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopColor, color_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightColor, color_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomColor, color_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftColor, color_value);
            }
        }
    });
}

void HTMLTableElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::cellpadding) {
        auto old_cellpadding = m_cellpadding;
        if (value.has_value())
            m_cellpadding = max(0, parse_integer(value.value()).value_or(0));
        else
            m_cellpadding = 1;

        // NOTE: cellpadding is magical, it applies to the cells inside this table, not the table itself.
        //       When it changes, we need new style for the cells.
        if (old_cellpadding != m_cellpadding) {
            for_each_in_subtree_of_type<HTMLTableCellElement>([&](auto& cell) {
                cell.set_needs_style_update(true);
                return TraversalDecision::Continue;
            });
        }
        return;
    }
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-caption
GC::Ptr<HTMLTableCaptionElement> HTMLTableElement::caption()
{
    // The caption IDL attribute must return, on getting, the first caption element child of the table element,
    // if any, or null otherwise.
    return first_child_of_type<HTMLTableCaptionElement>();
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-caption
WebIDL::ExceptionOr<void> HTMLTableElement::set_caption(HTMLTableCaptionElement* caption)
{
    // On setting, the first caption element child of the table element, if any, must be removed,
    // and the new value, if not null, must be inserted as the first node of the table element.
    delete_caption();

    if (caption)
        TRY(pre_insert(*caption, first_child()));

    return {};
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-createcaption
GC::Ref<HTMLTableCaptionElement> HTMLTableElement::create_caption()
{
    auto maybe_caption = caption();
    if (maybe_caption) {
        return *maybe_caption;
    }

    auto caption = DOM::create_element(document(), TagNames::caption, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(pre_insert(caption, first_child()));
    return static_cast<HTMLTableCaptionElement&>(*caption);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-deletecaption
void HTMLTableElement::delete_caption()
{
    auto maybe_caption = caption();
    if (maybe_caption) {
        maybe_caption->remove(false);
    }
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-thead
GC::Ptr<HTMLTableSectionElement> HTMLTableElement::t_head()
{
    // The tHead IDL attribute must return, on getting, the first thead element child of the table element,
    // if any, or null otherwise.
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (is<HTMLTableSectionElement>(*child)) {
            auto table_section_element = &as<HTMLTableSectionElement>(*child);
            if (table_section_element->local_name() == TagNames::thead)
                return table_section_element;
        }
    }

    return nullptr;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-thead
WebIDL::ExceptionOr<void> HTMLTableElement::set_t_head(HTMLTableSectionElement* thead)
{
    // If the new value is neither null nor a thead element, then a "HierarchyRequestError" DOMException must be thrown instead.
    if (thead && thead->local_name() != TagNames::thead)
        return WebIDL::HierarchyRequestError::create(realm(), "Element is not thead"_string);

    // On setting, if the new value is null or a thead element, the first thead element child of the table element,
    // if any, must be removed,
    delete_t_head();

    if (!thead)
        return {};

    // and the new value, if not null, must be inserted immediately before the first element in the table element
    // that is neither a caption element nor a colgroup element, if any,
    // or at the end of the table if there are no such elements.

    // We insert the new thead after any <caption> or <colgroup> elements
    DOM::Node* child_to_insert_before = nullptr;
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (!is<HTMLElement>(*child))
            continue;
        if (is<HTMLTableCaptionElement>(*child))
            continue;
        if (is<HTMLTableColElement>(*child)) {
            auto table_col_element = &as<HTMLTableColElement>(*child);
            if (table_col_element->local_name() == TagNames::colgroup)
                continue;
        }

        // We have found an element which is not a <caption> or <colgroup>, we'll insert before this
        child_to_insert_before = child;
        break;
    }

    TRY(pre_insert(*thead, child_to_insert_before));

    return {};
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-createthead
GC::Ref<HTMLTableSectionElement> HTMLTableElement::create_t_head()
{
    auto maybe_thead = t_head();
    if (maybe_thead)
        return *maybe_thead;

    auto thead = DOM::create_element(document(), TagNames::thead, Namespace::HTML).release_value_but_fixme_should_propagate_errors();

    // We insert the new thead after any <caption> or <colgroup> elements
    DOM::Node* child_to_insert_before = nullptr;
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (!is<HTMLElement>(*child))
            continue;
        if (is<HTMLTableCaptionElement>(*child))
            continue;
        if (is<HTMLTableColElement>(*child)) {
            auto table_col_element = &as<HTMLTableColElement>(*child);
            if (table_col_element->local_name() == TagNames::colgroup)
                continue;
        }

        // We have found an element which is not a <caption> or <colgroup>, we'll insert before this
        child_to_insert_before = child;
        break;
    }

    MUST(pre_insert(thead, child_to_insert_before));

    return static_cast<HTMLTableSectionElement&>(*thead);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-deletethead
void HTMLTableElement::delete_t_head()
{
    auto maybe_thead = t_head();
    if (maybe_thead) {
        maybe_thead->remove(false);
    }
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-tfoot
GC::Ptr<HTMLTableSectionElement> HTMLTableElement::t_foot()
{
    // The tFoot IDL attribute must return, on getting, the first tfoot element child of the table element,
    // if any, or null otherwise.
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (is<HTMLTableSectionElement>(*child)) {
            auto table_section_element = &as<HTMLTableSectionElement>(*child);
            if (table_section_element->local_name() == TagNames::tfoot)
                return table_section_element;
        }
    }

    return nullptr;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-tfoot
WebIDL::ExceptionOr<void> HTMLTableElement::set_t_foot(HTMLTableSectionElement* tfoot)
{
    // If the new value is neither null nor a tfoot element, then a "HierarchyRequestError" DOMException must be thrown instead.
    if (tfoot && tfoot->local_name() != TagNames::tfoot)
        return WebIDL::HierarchyRequestError::create(realm(), "Element is not tfoot"_string);

    // On setting, if the new value is null or a tfoot element, the first tfoot element child of the table element,
    // if any, must be removed,
    delete_t_foot();

    // and the new value, if not null, must be inserted at the end of the table.
    if (tfoot) {
        TRY(append_child(*tfoot));
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-createtfoot
GC::Ref<HTMLTableSectionElement> HTMLTableElement::create_t_foot()
{
    auto maybe_tfoot = t_foot();
    if (maybe_tfoot)
        return *maybe_tfoot;

    auto tfoot = DOM::create_element(document(), TagNames::tfoot, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(append_child(tfoot));
    return static_cast<HTMLTableSectionElement&>(*tfoot);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-deletetfoot
void HTMLTableElement::delete_t_foot()
{
    auto maybe_tfoot = t_foot();
    if (maybe_tfoot) {
        maybe_tfoot->remove(false);
    }
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-tbodies
GC::Ref<DOM::HTMLCollection> HTMLTableElement::t_bodies()
{
    // The tBodies attribute must return an HTMLCollection rooted at the table node,
    // whose filter matches only tbody elements that are children of the table element.
    if (!m_t_bodies) {
        m_t_bodies = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Children, [](DOM::Element const& element) {
            return element.local_name() == TagNames::tbody;
        });
    }
    return *m_t_bodies;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-createtbody
GC::Ref<HTMLTableSectionElement> HTMLTableElement::create_t_body()
{
    auto tbody = DOM::create_element(document(), TagNames::tbody, Namespace::HTML).release_value_but_fixme_should_propagate_errors();

    // We insert the new tbody after the last <tbody> element
    DOM::Node* child_to_insert_before = nullptr;
    for (auto* child = last_child(); child; child = child->previous_sibling()) {
        if (!is<HTMLElement>(*child))
            continue;
        if (is<HTMLTableSectionElement>(*child)) {
            auto table_section_element = &as<HTMLTableSectionElement>(*child);
            if (table_section_element->local_name() == TagNames::tbody) {
                // We have found an element which is a <tbody> we'll insert after this
                child_to_insert_before = child->next_sibling();
                break;
            }
        }
    }

    MUST(pre_insert(tbody, child_to_insert_before));

    return static_cast<HTMLTableSectionElement&>(*tbody);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-rows
GC::Ref<DOM::HTMLCollection> HTMLTableElement::rows()
{
    HTMLTableElement* table_node = this;
    // FIXME:  The elements in the collection must be ordered such that those elements whose parent is a thead are
    //         included first, in tree order, followed by those elements whose parent is either a table or tbody
    //         element, again in tree order, followed finally by those elements whose parent is a tfoot element,
    //         still in tree order.
    // How do you sort HTMLCollection?

    if (!m_rows) {
        m_rows = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [table_node](DOM::Element const& element) {
            // Only match TR elements which are:
            // * children of the table element
            // * children of the thead, tbody, or tfoot elements that are themselves children of the table element
            if (!is<HTMLTableRowElement>(element)) {
                return false;
            }
            if (element.parent_element() == table_node)
                return true;

            if (element.parent_element() && (element.parent_element()->local_name() == TagNames::thead || element.parent_element()->local_name() == TagNames::tbody || element.parent_element()->local_name() == TagNames::tfoot)
                && element.parent()->parent() == table_node) {
                return true;
            }

            return false;
        });
    }
    return *m_rows;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-insertrow
WebIDL::ExceptionOr<GC::Ref<HTMLTableRowElement>> HTMLTableElement::insert_row(WebIDL::Long index)
{
    auto rows = this->rows();
    auto rows_length = rows->length();

    if (index < -1 || index > (long)rows_length) {
        return WebIDL::IndexSizeError::create(realm(), "Index is negative or greater than the number of rows"_string);
    }
    auto& tr = static_cast<HTMLTableRowElement&>(*TRY(DOM::create_element(document(), TagNames::tr, Namespace::HTML)));
    if (rows_length == 0 && !has_child_of_type<HTMLTableRowElement>()) {
        auto tbody = TRY(DOM::create_element(document(), TagNames::tbody, Namespace::HTML));
        TRY(tbody->append_child(tr));
        TRY(append_child(tbody));
    } else if (rows_length == 0) {
        auto tbody = last_child_of_type<HTMLTableRowElement>();
        TRY(tbody->append_child(tr));
    } else if (index == -1 || index == (long)rows_length) {
        auto parent_of_last_tr = rows->item(rows_length - 1)->parent_element();
        TRY(parent_of_last_tr->append_child(tr));
    } else {
        rows->item(index)->parent_element()->insert_before(tr, rows->item(index));
    }
    return GC::Ref(tr);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-table-deleterow
WebIDL::ExceptionOr<void> HTMLTableElement::delete_row(WebIDL::Long index)
{
    auto rows = this->rows();
    auto rows_length = rows->length();

    // 1. If index is less than −1 or greater than or equal to the number of elements in the rows collection, then throw an "IndexSizeError" DOMException.
    if (index < -1 || index >= (long)rows_length)
        return WebIDL::IndexSizeError::create(realm(), "Index is negative or greater than or equal to the number of rows"_string);

    // 2. If index is −1, then remove the last element in the rows collection from its parent, or do nothing if the rows collection is empty.
    if (index == -1) {
        if (rows_length == 0)
            return {};

        auto row_to_remove = rows->item(rows_length - 1);
        row_to_remove->remove(false);
        return {};
    }

    // 3. Otherwise, remove the indexth element in the rows collection from its parent.
    auto row_to_remove = rows->item(index);
    row_to_remove->remove(false);
    return {};
}

unsigned int HTMLTableElement::border() const
{
    return parse_border(get_attribute_value(HTML::AttributeNames::border));
}

Optional<u32> HTMLTableElement::cellpadding() const
{
    return m_cellpadding;
}

}
