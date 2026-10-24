/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#ReadOnlyList
enum class ReadOnlyList : u8 {
    Yes,
    No,
};

// https://www.w3.org/TR/SVG2/types.html#TermListInterface
template<typename T>
class SVGList {
public:
    virtual ~SVGList() = default;

    // https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__length
    WebIDL::UnsignedLong length() const;
    WebIDL::UnsignedLong number_of_items() const { return length(); }

    WebIDL::ExceptionOr<void> clear();
    WebIDL::ExceptionOr<T> initialize_(T);
    WebIDL::ExceptionOr<T> get_item(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> insert_item_before(T, WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> replace_item(T, WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> remove_item(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> append_item(T);

    ReadonlySpan<T> items() { return m_items; }
    ReadonlySpan<T> items() const { return m_items; }

protected:
    SVGList(Vector<T>, ReadOnlyList);
    explicit SVGList(ReadOnlyList);

    void visit_edges(GC::Cell::Visitor& visitor);

    ReadOnlyList read_only() const { return m_read_only; }

    // https://svgwg.org/svg2-draft/types.html#TermAttach
    virtual void attach(T const&) { }

    // https://svgwg.org/svg2-draft/types.html#TermDetach
    virtual void detach(T const&) { }

    // If newItem is an object type, and newItem is not a detached object, then a newly created object of the same type
    // as newItem and which has the same value. Otherwise, newItem.
    virtual T copy_if_not_detached(T new_item) { return new_item; }

    // https://svgwg.org/svg2-draft/types.html#TermReserialize
    virtual void reserialize_reflected_attribute() { }

    Vector<T>& mutable_items() { return m_items; }

    // https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
    // The steps for synchronizing a list interface object whose list element type is an object type, given the values
    // of the reflected attribute, a function that creates a new item, and a function that sets an item's value.
    template<typename Values, typename CreateItem, typename SetItemValue>
    void synchronize_items(Values const& value, CreateItem&& create_item, SetItemValue&& set_item_value)
    {
        // 2. Let length be the number of items in the list.
        auto length = m_items.size();

        // 3. Let new length be the number of values in value.
        auto new_length = value.size();

        // 4. If the list element type is SVGNumber, SVGLength, DOMPoint or SVGTransform, then:
        //    1. If length > new length, then:
        if (length > new_length) {
            // 1. Detach each object in the list at an index greater than or equal to new length.
            for (size_t index = new_length; index < length; ++index)
                detach(m_items[index]);

            // 2. Truncate the list to length new length.
            m_items.shrink(new_length);

            // 3. Set length to new length.
            length = new_length;
        }

        // 2. While length < new length:
        while (length < new_length) {
            // 1. Let item be a newly created object of the list element type.
            T item = create_item();

            // 2. Attach item to this list interface object.
            attach(item);

            // 3. Append item to the list.
            m_items.append(item);

            // 4. Set length to length + 1.
            ++length;
        }

        // 3. Let index be 0.
        // 4. While index < length:
        for (size_t index = 0; index < length; ++index) {
            // 1. Let item be the object in the list at index index.
            // 2. Let v be the value in value at index index.
            // 3. Set item's value to v.
            // 4. If item is an SVGTransform object, then set the components of its matrix object to match the new
            //    transform function value.
            // 5. Set index to index + 1.
            set_item_value(m_items[index], value[index]);
        }
    }

private:
    Vector<T> m_items;

    // https://www.w3.org/TR/SVG2/types.html#ReadOnlyList
    ReadOnlyList m_read_only;
};

}
