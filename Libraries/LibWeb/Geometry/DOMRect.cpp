/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMRectPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRect);

WebIDL::ExceptionOr<GC::Ref<DOMRect>> DOMRect::construct_impl(JS::Realm& realm, double x, double y, double width, double height)
{
    return create(realm, Gfx::FloatRect { x, y, width, height });
}

GC::Ref<DOMRect> DOMRect::create(JS::Realm& realm, Gfx::FloatRect const& rect)
{
    return realm.create<DOMRect>(realm, rect.x(), rect.y(), rect.width(), rect.height());
}

GC::Ref<DOMRect> DOMRect::create(JS::Realm& realm)
{
    return realm.create<DOMRect>(realm);
}

// https://drafts.fxtf.org/geometry/#create-a-domrect-from-the-dictionary
GC::Ref<DOMRect> DOMRect::from_rect(JS::VM& vm, Geometry::DOMRectInit const& other)
{
    auto& realm = *vm.current_realm();
    return realm.create<DOMRect>(realm, other.x, other.y, other.width, other.height);
}

DOMRect::DOMRect(JS::Realm& realm, double x, double y, double width, double height)
    : DOMRectReadOnly(realm, x, y, width, height)
{
}

DOMRect::DOMRect(JS::Realm& realm)
    : DOMRectReadOnly(realm)
{
}

DOMRect::~DOMRect() = default;

void DOMRect::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMRect);
}

}
