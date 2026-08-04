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
Optional<Utf16String> DOMStringList::item(u32 index) const
{
    // The item(index) method steps are to return the indexth item in this's associated list, or null if index plus one
    // is greater than this's associated list's size.
    if (index >= m_list.size())
        return {};

    return Utf16String::from_utf8(m_list.at(index));
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-contains
bool DOMStringList::contains(Utf16View string)
{
    // The contains(string) method steps are to return true if this's associated list contains string, and false otherwise.
    return any_of(m_list, [&](auto const& item) {
        auto item_utf16 = Utf16String::from_utf8(item);
        return item_utf16.utf16_view() == string;
    });
}

}
