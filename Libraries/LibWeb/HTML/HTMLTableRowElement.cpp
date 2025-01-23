/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableRowElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/ParsingContext.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/HTMLTableSectionElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTableRowElement);

HTMLTableRowElement::HTMLTableRowElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableRowElement::~HTMLTableRowElement() = default;

void HTMLTableRowElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableRowElement);
}

bool HTMLTableRowElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::bgcolor,
        HTML::AttributeNames::background,
        HTML::AttributeNames::height,
        HTML::AttributeNames::valign);
}

void HTMLTableRowElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::bgcolor) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name == HTML::AttributeNames::background) {
            // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:encoding-parsing-and-serializing-a-url
            if (auto parsed_value = document().encoding_parse_url(value); parsed_value.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundImage, CSS::ImageStyleValue::create(*parsed_value));
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, *parsed_value);
        } else if (name == HTML::AttributeNames::valign) {
            if (auto parsed_value = parse_css_value(CSS::Parser::ParsingContext { document() }, value, CSS::PropertyID::VerticalAlign))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::VerticalAlign, parsed_value.release_nonnull());
        }
    });
}

void HTMLTableRowElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cells);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tr-cells
GC::Ref<DOM::HTMLCollection> HTMLTableRowElement::cells() const
{
    // The cells attribute must return an HTMLCollection rooted at this tr element,
    // whose filter matches only td and th elements that are children of the tr element.
    if (!m_cells) {
        m_cells = DOM::HTMLCollection::create(const_cast<HTMLTableRowElement&>(*this), DOM::HTMLCollection::Scope::Children, [](Element const& element) {
            return is<HTMLTableCellElement>(element);
        });
    }
    return *m_cells;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tr-rowindex
int HTMLTableRowElement::row_index() const
{
    // The rowIndex attribute must, if this element has a parent table element,
    // or a parent tbody, thead, or tfoot element and a grandparent table element,
    // return the index of this tr element in that table element's rows collection.
    // If there is no such table element, then the attribute must return −1.
    auto rows_collection = [&]() -> GC::Ptr<DOM::HTMLCollection> {
        if (!parent())
            return nullptr;
        if (is<HTMLTableElement>(*parent()))
            return const_cast<HTMLTableElement&>(static_cast<HTMLTableElement const&>(*parent())).rows();
        if (is<HTMLTableSectionElement>(*parent()) && parent()->parent() && is<HTMLTableElement>(*parent()->parent()))
            return const_cast<HTMLTableElement&>(static_cast<HTMLTableElement const&>(*parent()->parent())).rows();
        return nullptr;
    }();
    if (!rows_collection)
        return -1;
    auto rows = rows_collection->collect_matching_elements();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] == this)
            return i;
    }
    return -1;
}

int HTMLTableRowElement::section_row_index() const
{
    // The sectionRowIndex attribute must, if this element has a parent table, tbody, thead, or tfoot element,
    // return the index of the tr element in the parent element's rows collection
    // (for tables, that's HTMLTableElement's rows collection; for table sections, that's HTMLTableSectionElement's rows collection).
    // If there is no such parent element, then the attribute must return −1.
    auto rows_collection = [&]() -> GC::Ptr<DOM::HTMLCollection> {
        if (!parent())
            return nullptr;
        if (is<HTMLTableElement>(*parent()))
            return const_cast<HTMLTableElement&>(static_cast<HTMLTableElement const&>(*parent())).rows();
        if (is<HTMLTableSectionElement>(*parent()))
            return static_cast<HTMLTableSectionElement const&>(*parent()).rows();
        return nullptr;
    }();
    if (!rows_collection)
        return -1;
    auto rows = rows_collection->collect_matching_elements();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] == this)
            return i;
    }
    return -1;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tr-insertcell
WebIDL::ExceptionOr<GC::Ref<HTMLTableCellElement>> HTMLTableRowElement::insert_cell(i32 index)
{
    auto cells_collection = cells();
    auto cells_collection_size = static_cast<i32>(cells_collection->length());

    // 1. If index is less than −1 or greater than the number of elements in the cells collection, then throw an "IndexSizeError" DOMException.
    if (index < -1 || index > cells_collection_size)
        return WebIDL::IndexSizeError::create(realm(), "Index is negative or greater than the number of cells"_string);

    // 2. Let table cell be the result of creating an element given this tr element's node document, "td", and the HTML namespace.
    auto& table_cell = static_cast<HTMLTableCellElement&>(*TRY(DOM::create_element(document(), HTML::TagNames::td, Namespace::HTML)));

    // 3. If index is equal to −1 or equal to the number of items in cells collection, then append table cell to this tr element.
    if (index == -1 || index == cells_collection_size)
        TRY(append_child(table_cell));

    // 4. Otherwise, insert table cell as a child of this tr element, immediately before the indexth td or th element in the cells collection.
    else
        insert_before(table_cell, cells_collection->item(index));

    // 5. Return table cell.
    return GC::Ref(table_cell);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tr-deletecell
WebIDL::ExceptionOr<void> HTMLTableRowElement::delete_cell(i32 index)
{
    auto cells_collection = cells();
    auto cells_collection_size = static_cast<i32>(cells_collection->length());

    // 1. If index is less than −1 or greater than or equal to the number of elements in the cells collection, then throw an "IndexSizeError" DOMException.
    if (index < -1 || index >= cells_collection_size)
        return WebIDL::IndexSizeError::create(realm(), "Index is negative or greater than or equal to the number of cells"_string);

    // 2. If index is −1, then remove the last element in the cells collection from its parent, or do nothing if the cells collection is empty.
    if (index == -1) {
        if (cells_collection_size > 0)
            cells_collection->item(cells_collection_size - 1)->remove();
    }

    // 3. Otherwise, remove the indexth element in the cells collection from its parent.
    else {
        cells_collection->item(index)->remove();
    }

    return {};
}

}
