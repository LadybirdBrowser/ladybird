/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableSectionElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/HTMLTableSectionElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTableSectionElement);

HTMLTableSectionElement::HTMLTableSectionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableSectionElement::~HTMLTableSectionElement() = default;

void HTMLTableSectionElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableSectionElement);
}

void HTMLTableSectionElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rows);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tbody-rows
GC::Ref<DOM::HTMLCollection> HTMLTableSectionElement::rows() const
{
    // The rows attribute must return an HTMLCollection rooted at this element,
    // whose filter matches only tr elements that are children of this element.
    if (!m_rows) {
        m_rows = DOM::HTMLCollection::create(const_cast<HTMLTableSectionElement&>(*this), DOM::HTMLCollection::Scope::Children, [](Element const& element) {
            return is<HTMLTableRowElement>(element);
        });
    }
    return *m_rows;
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tbody-insertrow
WebIDL::ExceptionOr<GC::Ref<HTMLTableRowElement>> HTMLTableSectionElement::insert_row(WebIDL::Long index)
{
    auto rows_collection = rows();
    auto rows_collection_size = static_cast<long>(rows_collection->length());

    // 1. If index is less than −1 or greater than the number of elements in the rows collection, throw an "IndexSizeError" DOMException.
    if (index < -1 || index > rows_collection_size)
        return WebIDL::IndexSizeError::create(realm(), "Index is negative or greater than the number of rows"_string);

    // 2. Let table row be the result of creating an element given this element's node document, "tr", and the HTML namespace.
    auto& table_row = static_cast<HTMLTableRowElement&>(*TRY(DOM::create_element(document(), TagNames::tr, Namespace::HTML)));

    // 3. If index is −1 or equal to the number of items in the rows collection, then append table row to this element.
    if (index == -1 || index == rows_collection_size)
        TRY(append_child(table_row));
    // 4. Otherwise, insert table row as a child of this element, immediately before the index-th tr element in the rows collection.
    else
        insert_before(table_row, rows_collection->item(index));

    // 5. Return table row.
    return GC::Ref(table_row);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-tbody-deleterow
WebIDL::ExceptionOr<void> HTMLTableSectionElement::delete_row(WebIDL::Long index)
{
    auto rows_collection = rows();
    auto rows_collection_size = static_cast<long>(rows_collection->length());

    // 1. If index is less than −1 or greater than or equal to the number of elements in the rows collection, then throw an "IndexSizeError" DOMException.
    if (index < -1 || index >= rows_collection_size)
        return WebIDL::IndexSizeError::create(realm(), "Index is negative or greater than or equal to the number of rows"_string);

    // 2. If index is −1, then remove the last element in the rows collection from this element, or do nothing if the rows collection is empty.
    if (index == -1) {
        if (rows_collection_size > 0)
            rows_collection->item(rows_collection_size - 1)->remove();
    }
    // 3. Otherwise, remove the indexth element in the rows collection from this element.
    else {
        rows_collection->item(index)->remove();
    }
    return {};
}

bool HTMLTableSectionElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::background,
        HTML::AttributeNames::bgcolor,
        HTML::AttributeNames::height);
}

void HTMLTableSectionElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:encoding-parsing-and-serializing-a-url
        if (name == HTML::AttributeNames::background) {
            if (auto parsed_value = document().encoding_parse_url(value); parsed_value.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundImage, CSS::ImageStyleValue::create(*parsed_value));
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:rules-for-parsing-a-legacy-colour-value
        else if (name == HTML::AttributeNames::bgcolor) {
            if (auto color = parse_legacy_color_value(value); color.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, parsed_value.release_nonnull());
        }
    });
}

}
