/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibWeb/DOM/AttributeList.h>

namespace Web::DOM {

NonnullOwnPtr<AttributeList> AttributeList::create(size_t capacity)
{
    VERIFY(capacity > 0 && capacity <= NumericLimits<u32>::max());
    auto allocation_size = Checked<size_t>(capacity);
    allocation_size *= sizeof(Attribute);
    allocation_size += sizeof(AttributeList);
    auto* memory = AttributeList::operator new(allocation_size.value());
    return adopt_own(*new (memory) AttributeList(static_cast<u32>(capacity)));
}

AttributeList::~AttributeList()
{
    // NB: libweb_rust reads the attributes in place, with a mirror of this layout (see Rust/src/dom/element_core.rs).
    static_assert(sizeof(AttributeList) == 8 && alignof(AttributeList) == 4);
    static_assert(offsetof(AttributeList, m_size) == 0);
    static_assert(offsetof(AttributeList, m_capacity) == 4);
    static_assert(sizeof(Attribute) == 16 && alignof(Attribute) == 8);
    static_assert(offsetof(Attribute, name) == 0);
    static_assert(offsetof(Attribute, value) == 8);

    for (auto& attribute : span())
        attribute.~Attribute();
}

NonnullOwnPtr<AttributeList> AttributeList::ensure_room_for_one_more(NonnullOwnPtr<AttributeList> list)
{
    if (list->m_size < list->m_capacity)
        return list;
    return list->moved_into_new_list(max(static_cast<size_t>(list->m_capacity) * 2, static_cast<size_t>(4)));
}

NonnullOwnPtr<AttributeList> AttributeList::moved_into_new_list(size_t capacity)
{
    VERIFY(capacity >= m_size);
    auto new_list = create(capacity);
    for (auto& attribute : span())
        new (&new_list->attributes()[new_list->m_size++]) Attribute(move(attribute));
    return new_list;
}

void AttributeList::unchecked_append(QualifiedName name, Utf16String value)
{
    VERIFY(m_size < m_capacity);
    new (&attributes()[m_size++]) Attribute(move(name), move(value));
}

void AttributeList::remove(size_t index)
{
    VERIFY(index < m_size);
    auto* attributes = this->attributes();
    for (size_t i = index; i + 1 < m_size; ++i)
        attributes[i] = move(attributes[i + 1]);
    attributes[--m_size].~Attribute();
}

}
