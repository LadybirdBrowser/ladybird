/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AttributeList.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

AttributeList::~AttributeList()
{
    // NB: libweb_rust defines this layout (see Rust/src/dom/element_core.rs), which it asserts too.
    static_assert(sizeof(AttributeList) == 8 && alignof(AttributeList) == 8);
    static_assert(sizeof(Header) == 8 && alignof(Header) == 4);
    static_assert(offsetof(Header, size) == 0);
    static_assert(offsetof(Header, capacity) == 4);
    static_assert(sizeof(Attribute) == 16 && alignof(Attribute) == 8);
    static_assert(offsetof(Attribute, name) == 0);
    static_assert(offsetof(Attribute, value) == 8);
    static_assert(RustFFI::ATTRIBUTE_LIST_PREFIX_SIZE == 8);

    if (m_list)
        RustFFI::rust_dom_attribute_list_destroy(this);
}

size_t AttributeList::capacity() const
{
    return m_list ? (m_list->capacity & ~RustFFI::ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP) : 0;
}

size_t AttributeList::allocation_size() const
{
    if (!m_list)
        return 0;
    return (attribute_map_slot() ? RustFFI::ATTRIBUTE_LIST_PREFIX_SIZE : 0) + sizeof(Header) + capacity() * sizeof(Attribute);
}

GC::Member<NamedNodeMap>* AttributeList::attribute_map_slot() const
{
    if (!m_list || !(m_list->capacity & RustFFI::ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP))
        return nullptr;
    // NB: The list keeps the field just before its header.
    return reinterpret_cast<GC::Member<NamedNodeMap>*>(reinterpret_cast<u8*>(m_list) - RustFFI::ATTRIBUTE_LIST_PREFIX_SIZE);
}

}
