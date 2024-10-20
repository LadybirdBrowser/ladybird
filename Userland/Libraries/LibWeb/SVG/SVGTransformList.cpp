/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGTransformListPrototype.h>
#include <LibWeb/SVG/SVGTransformList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGTransformList);

JS::NonnullGCPtr<SVGTransformList> SVGTransformList::create(JS::Realm& realm)
{
    return realm.heap().allocate<SVGTransformList>(realm, realm);
}

SVGTransformList::~SVGTransformList() = default;

SVGTransformList::SVGTransformList(JS::Realm& realm)
    : PlatformObject(realm)
{
}

// https://svgwg.org/svg2-draft/single-page.html#types-__svg__SVGNameList__length
WebIDL::UnsignedLong SVGTransformList::length()
{
    // The length and numberOfItems IDL attributes represents the length of the list, and on getting simply return the length of the list.
    return m_transforms.size();
}

// https://svgwg.org/svg2-draft/single-page.html#types-__svg__SVGNameList__numberOfItems
WebIDL::UnsignedLong SVGTransformList::number_of_items()
{
    // The length and numberOfItems IDL attributes represents the length of the list, and on getting simply return the length of the list.
    return m_transforms.size();
}

// https://svgwg.org/svg2-draft/single-page.html#types-__svg__SVGNameList__getItem
WebIDL::ExceptionOr<JS::NonnullGCPtr<SVGTransform>> SVGTransformList::get_item(WebIDL::UnsignedLong index)
{
    // 1. If index is greater than or equal to the length of the list, then throw an IndexSizeError.
    if (index >= m_transforms.size())
        return WebIDL::IndexSizeError::create(realm(), "SVGTransformList index out of bounds"_string);
    // 2. Return the element in the list at position index.
    return m_transforms.at(index);
}

// https://svgwg.org/svg2-draft/single-page.html#types-__svg__SVGNameList__appendItem
JS::NonnullGCPtr<SVGTransform> SVGTransformList::append_item(JS::NonnullGCPtr<SVGTransform> new_item)
{
    // FIXME: This does not implement the steps from the specification.
    m_transforms.append(new_item);
    return new_item;
}

void SVGTransformList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTransformList);
}

void SVGTransformList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto transform : m_transforms)
        transform->visit_edges(visitor);
}

}
