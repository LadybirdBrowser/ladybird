/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumberList);

GC::Ref<SVGNumberList> SVGNumberList::create(Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGNumberList>(move(items), read_only);
}

GC::Ref<SVGNumberList> SVGNumberList::create(ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGNumberList>(read_only);
}

SVGNumberList::SVGNumberList(Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
    : SVGList(move(items), read_only)
{
}

SVGNumberList::SVGNumberList(ReadOnlyList read_only)
    : SVGList(read_only)
{
}

Optional<JS::Value> SVGNumberList::item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const
{
    if (index >= items().size())
        return {};
    return Bindings::wrap(wrapper_world, realm, items()[index]);
}

static WebIDL::ExceptionOr<GC::Ref<SVGNumber>> svg_number_from_value(JS::Value value)
{
    if (value.is_object()) {
        if (auto* number = Bindings::impl_from<SVGNumber>(&value.as_object()))
            return GC::Ref { *number };
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value must be an SVGNumber"sv };
}

WebIDL::ExceptionOr<void> SVGNumberList::set_value_of_new_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_number_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGNumberList::set_value_of_existing_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_number_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGNumberList::set_value_of_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_number_from_value(value)), index));
    return {};
}

void SVGNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
