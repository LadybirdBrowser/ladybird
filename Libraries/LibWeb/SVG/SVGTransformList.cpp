/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGTransform.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTransformList);

GC::Ref<SVGTransformList> SVGTransformList::create(Vector<GC::Ref<SVGTransform>> items, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGTransformList>(move(items), read_only);
}

GC::Ref<SVGTransformList> SVGTransformList::create(ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGTransformList>(read_only);
}

SVGTransformList::SVGTransformList(Vector<GC::Ref<SVGTransform>> items, ReadOnlyList read_only)
    : SVGList(move(items), read_only)
{
}

SVGTransformList::SVGTransformList(ReadOnlyList read_only)
    : SVGList(read_only)
{
}

Optional<JS::Value> SVGTransformList::item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const
{
    if (index >= items().size())
        return {};
    return Bindings::wrap(wrapper_world, realm, items()[index]);
}

static WebIDL::ExceptionOr<GC::Ref<SVGTransform>> svg_transform_from_value(JS::Value value)
{
    if (value.is_object()) {
        if (auto* transform = Bindings::impl_from<SVGTransform>(&value.as_object()))
            return GC::Ref { *transform };
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value must be an SVGTransform"sv };
}

WebIDL::ExceptionOr<void> SVGTransformList::set_value_of_new_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_transform_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGTransformList::set_value_of_existing_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_transform_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGTransformList::set_value_of_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_transform_from_value(value)), index));
    return {};
}

void SVGTransformList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
