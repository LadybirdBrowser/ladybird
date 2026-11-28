/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/QualifiedName.h>

namespace Web::DOM {

// The attributes of an element, in order, in the same allocation as their count and capacity. C++ alone constructs,
// changes and destroys them. libweb_rust reads them in place (see Rust/src/dom/element_core.rs), and mirrors this
// layout, which AttributeList.cpp asserts.
class AttributeList {
    AK_MAKE_NONCOPYABLE(AttributeList);
    AK_MAKE_NONMOVABLE(AttributeList);
    AK_ALLOC_WITH_KMALLOC;

public:
    struct Attribute {
        QualifiedName name;
        Utf16String value;
    };

    static NonnullOwnPtr<AttributeList> create(size_t capacity);
    ~AttributeList();

    // Returns a list with room for one more attribute: this one, or a new one these attributes were moved into.
    static NonnullOwnPtr<AttributeList> ensure_room_for_one_more(NonnullOwnPtr<AttributeList>);
    NonnullOwnPtr<AttributeList> moved_into_new_list(size_t capacity);

    void unchecked_append(QualifiedName, Utf16String value);
    void remove(size_t index);

    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    bool is_empty() const { return m_size == 0; }

    Attribute& at(size_t index)
    {
        VERIFY(index < m_size);
        return attributes()[index];
    }
    Attribute const& at(size_t index) const
    {
        VERIFY(index < m_size);
        return attributes()[index];
    }
    Attribute& last() { return at(m_size - 1); }

    Span<Attribute> span() { return { attributes(), m_size }; }
    ReadonlySpan<Attribute> span() const { return { attributes(), m_size }; }

private:
    explicit AttributeList(u32 capacity)
        : m_capacity(capacity)
    {
    }

    // The attributes follow the list in its allocation.
    Attribute* attributes() { return reinterpret_cast<Attribute*>(this + 1); }
    Attribute const* attributes() const { return reinterpret_cast<Attribute const*>(this + 1); }

    u32 m_size { 0 };
    u32 m_capacity { 0 };
};

}
