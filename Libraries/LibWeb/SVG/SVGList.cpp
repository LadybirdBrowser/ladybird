/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

template<typename T>
SVGList<T>::SVGList(JS::Realm& realm, Vector<T> items, ReadOnlyList read_only)
    : m_realm(realm)
    , m_items(move(items))
    , m_read_only(read_only)
{
}

template<typename T>
SVGList<T>::SVGList(JS::Realm& realm, ReadOnlyList read_only)
    : m_realm(realm)
    , m_read_only(read_only)
{
}

template<typename T>
void SVGList<T>::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_realm);
    visitor.visit(m_items);
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__length
template<typename T>
WebIDL::UnsignedLong SVGList<T>::length() const
{
    // The length and numberOfItems IDL attributes represents the length of the list, and on getting simply return the
    // length of the list.
    return m_items.size();
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__clear
template<typename T>
WebIDL::ExceptionOr<void> SVGList<T>::clear()
{
    // 1. If the list is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create(m_realm, "Cannot modify a read-only list"_utf16);

    // 2. Detach and then remove all elements in the list.
    // FIXME: Detach items.
    m_items.clear();

    // FIXME: 3. If the list reflects an attribute, or represents the base value of an object that reflects an attribute, then
    //    reserialize the reflected attribute.

    return {};
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__initialize
template<typename T>
WebIDL::ExceptionOr<T> SVGList<T>::initialize_(T new_item)
{
    // 1. If the list is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create(m_realm, "Cannot modify a read-only list"_utf16);

    // 2. Detach and then remove all elements in the list.
    // FIXME: Detach items.
    m_items.clear();

    // FIXME: 3. If newItem is an object type, and newItem is not a detached object, then set newItem to be a newly created
    //    object of the same type as newItem and which has the same (number or length) value.

    // FIXME: 4. Attach newItem to the list interface object.

    // 5. Append newItem to this list.
    m_items.append(new_item);

    // FIXME: 6. If the list reflects an attribute, or represents the base value of an object that reflects an attribute, then
    //    reserialize the reflected attribute.

    // 7. Return newItem.
    return new_item;
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__getItem
template<typename T>
WebIDL::ExceptionOr<T> SVGList<T>::get_item(WebIDL::UnsignedLong index)
{
    // 1. If index is greater than or equal to the length of the list, then throw an IndexSizeError.
    if (index >= m_items.size())
        return WebIDL::IndexSizeError::create(m_realm, "List index out of bounds"_utf16);

    // 2. Return the element in the list at position index.
    return m_items[index];
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__insertItemBefore
template<typename T>
WebIDL::ExceptionOr<T> SVGList<T>::insert_item_before(T new_item, WebIDL::UnsignedLong index)
{
    // 1. If the list is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create(m_realm, "Cannot modify a read-only list"_utf16);

    // FIXME: 2. If newItem is an object type, and newItem is not a detached object, then set newItem to be a newly created
    //    object of the same type as newItem and which has the same (number or length) value.

    // 3. If index is greater than the length of the list, then set index to be the list length.
    if (index > m_items.size())
        index = m_items.size();

    // 4. Insert newItem into the list at index index.
    m_items.insert(index, new_item);

    // FIXME: 5. Attach newItem to the list interface object.

    // FIXME: 6. If the list reflects an attribute, or represents the base value of an object that reflects an attribute, then
    //    reserialize the reflected attribute.

    // 7. Return newItem.
    return new_item;
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__replaceItem
template<typename T>
WebIDL::ExceptionOr<T> SVGList<T>::replace_item(T new_item, WebIDL::UnsignedLong index)
{
    // 1. If the list is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create(m_realm, "Cannot modify a read-only list"_utf16);

    // 2. If index is greater than or equal to the length of the list, then throw an IndexSizeError.
    if (index >= m_items.size())
        return WebIDL::IndexSizeError::create(m_realm, "List index out of bounds"_utf16);

    // FIXME: 3. If newItem is an object type, and newItem is not a detached object, then set newItem to be a newly created
    //    object of the same type as newItem and which has the same (number or length) value.

    // FIXME: 4. Detach the element in the list at index index.

    // 5. Replace the element in the list at index index with newItem.
    m_items[index] = new_item;

    // FIXME: 6. Attach newItem to the list interface object.

    // FIXNE: 7. If the list reflects an attribute, or represents the base value of an object that reflects an attribute, then
    //    reserialize the reflected attribute.

    // 8. Return newItem.
    return new_item;
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__removeItem
template<typename T>
WebIDL::ExceptionOr<T> SVGList<T>::remove_item(WebIDL::UnsignedLong index)
{
    // 1. If the list is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create(m_realm, "Cannot modify a read-only list"_utf16);

    // 2. If index is greater than or equal to the length of the list, then throw an IndexSizeError with code.
    if (index >= m_items.size())
        return WebIDL::IndexSizeError::create(m_realm, "List index out of bounds"_utf16);

    // 3. Let item be the list element at index index.
    auto item = m_items[index];

    // FIXME: 4. Detach item.

    // 5. Remove the list element at index index.
    m_items.remove(index);

    // 6. Return item.
    return item;
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__appendItem
template<typename T>
WebIDL::ExceptionOr<T> SVGList<T>::append_item(T new_item)
{
    // 1. If the list is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create(m_realm, "Cannot modify a read-only list"_utf16);

    // FIXME: 2. If newItem is an object type, and newItem is not a detached object, then set newItem to be a newly created
    //    object of the same type as newItem and which has the same (number or length) value.

    // 3. Let index be the length of the list.
    // AD-HOC: No, this is unused.

    // 4. Append newItem to the end of the list.
    m_items.append(new_item);

    // FIXME: 5. Attach newItem to the list interface object.

    // FIXME: 6. If the list reflects an attribute, or represents the base value of an object that reflects an attribute, then
    //    reserialize the reflected attribute.

    // 7. Return newItem.
    return new_item;
}

template class SVGList<GC::Ref<SVGNumber>>;
template class SVGList<GC::Ref<SVGTransform>>;

}
