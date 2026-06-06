/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumberList);

GC::Ref<SVGNumberList> SVGNumberList::create(JS::Realm& realm, Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
{
    return realm.create<SVGNumberList>(realm, move(items), read_only);
}

GC::Ref<SVGNumberList> SVGNumberList::create(JS::Realm& realm, ReadOnlyList read_only)
{
    return realm.create<SVGNumberList>(realm, read_only);
}

SVGNumberList::SVGNumberList(JS::Realm& realm, Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
    : Bindings::Wrappable(realm)
    , SVGList(move(items), read_only)
{
}

SVGNumberList::SVGNumberList(JS::Realm& realm, ReadOnlyList read_only)
    : Bindings::Wrappable(realm)
    , SVGList(read_only)
{
}

Optional<JS::Value> SVGNumberList::item_value(JS::Realm& realm, size_t index) const
{
    if (index >= items().size())
        return {};
    return Bindings::wrap(realm, items()[index]);
}

static WebIDL::ExceptionOr<GC::Ref<SVGNumber>> svg_number_from_value(JS::Value value)
{
    if (value.is_object()) {
        if (auto* number = Bindings::impl_from<SVGNumber>(&value.as_object()))
            return GC::Ref { *number };
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value must be an SVGNumber"sv };
}

WebIDL::ExceptionOr<void> SVGNumberList::set_value_of_new_indexed_property(u32 index, JS::Value value)
{
    TRY(replace_item(TRY(svg_number_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGNumberList::set_value_of_existing_indexed_property(u32 index, JS::Value value)
{
    TRY(replace_item(TRY(svg_number_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGNumberList::set_value_of_indexed_property(u32 index, JS::Value value)
{
    TRY(replace_item(TRY(svg_number_from_value(value)), index));
    return {};
}

void SVGNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
