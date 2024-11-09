/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMStringListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DOMStringList);

JS::NonnullGCPtr<DOMStringList> DOMStringList::create(JS::Realm& realm, Vector<String> list)
{
    return realm.heap().allocate<DOMStringList>(realm, realm, list);
}

DOMStringList::DOMStringList(JS::Realm& realm, Vector<String> list)
    : Bindings::PlatformObject(realm)
    , m_list(move(list))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = true };
}

void DOMStringList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMStringList);
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
    if (index + 1 > m_list.size())
        return {};

    return m_list.at(index);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-contains
bool DOMStringList::contains(StringView string)
{
    // The contains(string) method steps are to return true if this's associated list contains string, and false otherwise.
    return m_list.contains_slow(string);
}

Optional<JS::Value> DOMStringList::item_value(size_t index) const
{
    if (index + 1 > m_list.size())
        return {};

    return JS::PrimitiveString::create(vm(), m_list.at(index));
}

}
