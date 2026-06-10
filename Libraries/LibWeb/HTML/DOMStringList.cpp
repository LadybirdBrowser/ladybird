/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DOMStringList);

GC::Ref<DOMStringList> DOMStringList::create(Vector<String> list)
{
    return GC::Heap::the().allocate<DOMStringList>(move(list));
}

DOMStringList::DOMStringList(Vector<String> list)
    : m_list(move(list))
{
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-length
u32 DOMStringList::length() const
{
    // The length getter steps are to return this's associated list's size.
    return m_list.size();
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-item
Optional<String> DOMStringList::item(u32 index) const
{
    // The item(index) method steps are to return the indexth item in this's associated list, or null if index plus one
    // is greater than this's associated list's size.
    if (index >= m_list.size())
        return {};

    return m_list.at(index);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-contains
bool DOMStringList::contains(StringView string)
{
    // The contains(string) method steps are to return true if this's associated list contains string, and false otherwise.
    return m_list.contains_slow(string);
}

}
