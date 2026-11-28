/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Span.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// The attributes of an element, in order, as ElementCore holds them: a pointer to their count and capacity, which they
// follow in one allocation, or null for an element that never had an attribute. libweb_rust allocates, changes and
// frees the list (see Rust/src/dom/element_core.rs), and defines its layout, which AttributeList.cpp asserts. C++ only
// reads it, and has Rust free it as it destroys the element.
class WEB_API AttributeList {
    AK_MAKE_NONCOPYABLE(AttributeList);
    AK_MAKE_NONMOVABLE(AttributeList);

public:
    // An attribute, which only libweb_rust constructs, changes and destroys.
    struct Attribute {
        Attribute() = delete;
        ~Attribute() = delete;

        QualifiedName name;
        Utf16String value;
    };

    AttributeList() = default;
    ~AttributeList();

    size_t size() const { return m_list ? m_list->size : 0; }
    bool is_empty() const { return size() == 0; }
    size_t capacity() const;

    Attribute const& at(size_t index) const
    {
        VERIFY(index < size());
        return attributes()[index];
    }

    ReadonlySpan<Attribute> span() const
    {
        if (!m_list)
            return {};
        return { attributes(), m_list->size };
    }

    void const* memory() const { return m_list; }
    size_t allocation_size() const;

    // The field of the element's NamedNodeMap, which the list has once script asked for the map, or null.
    GC::Member<NamedNodeMap>* attribute_map_slot() const;

private:
    struct Header {
        u32 size;
        u32 capacity;
    };

    // The attributes follow the list's header in its allocation.
    Attribute const* attributes() const { return reinterpret_cast<Attribute const*>(m_list + 1); }

    Header* m_list { nullptr };
};

}
