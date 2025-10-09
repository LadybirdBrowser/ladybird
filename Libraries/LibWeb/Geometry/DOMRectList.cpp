/*
 * Copyright (c) 2022, DerpyCrabs <derpycrabs@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Root.h>
#include <LibWeb/Bindings/DOMRectListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRectList);

GC::Ref<DOMRectList> DOMRectList::create(JS::Realm& realm, Vector<GC::Root<DOMRect>> const& rect_handles)
{
    Vector<GC::Ref<DOMRect>> rects;
    for (auto& rect : rect_handles)
        rects.append(*rect);
    return realm.create<DOMRectList>(realm, move(rects));
}

DOMRectList::DOMRectList(JS::Realm& realm, Vector<GC::Ref<DOMRect>> rects)
    : Bindings::PlatformObject(realm)
    , m_rects(move(rects))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = 1 };
}

DOMRectList::~DOMRectList() = default;

void DOMRectList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMRectList);
    Base::initialize(realm);
}

void DOMRectList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rects);
}

// https://drafts.fxtf.org/geometry-1/#dom-domrectlist-length
u32 DOMRectList::length() const
{
    return m_rects.size();
}

// https://drafts.fxtf.org/geometry-1/#dom-domrectlist-item
DOMRect const* DOMRectList::item(u32 index) const
{
    // The item(index) method, when invoked, must return null when
    // index is greater than or equal to the number of DOMRect objects associated with the DOMRectList.
    // Otherwise, the DOMRect object at index must be returned. Indices are zero-based.
    if (index >= m_rects.size())
        return nullptr;
    return m_rects[index];
}

Optional<JS::Value> DOMRectList::item_value(size_t index) const
{
    if (index >= m_rects.size())
        return {};

    return m_rects[index].ptr();
}

}
