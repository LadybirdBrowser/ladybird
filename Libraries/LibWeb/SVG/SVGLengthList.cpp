/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLengthList);

GC::Ref<SVGLengthList> SVGLengthList::create(JS::Realm& realm, Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
{
    return realm.create<SVGLengthList>(realm, move(items), read_only);
}

GC::Ref<SVGLengthList> SVGLengthList::create(JS::Realm& realm, ReadOnlyList read_only)
{
    return realm.create<SVGLengthList>(realm, read_only);
}

SVGLengthList::SVGLengthList(JS::Realm& realm, Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
    : Bindings::Wrappable(realm)
    , SVGList(move(items), read_only)
{
}

SVGLengthList::SVGLengthList(JS::Realm& realm, ReadOnlyList read_only)
    : Bindings::Wrappable(realm)
    , SVGList(read_only)
{
}

Optional<JS::Value> SVGLengthList::item_value(JS::Realm& realm, size_t index) const
{
    if (index >= items().size())
        return {};
    return Bindings::wrap(realm, items()[index]);
}

static WebIDL::ExceptionOr<GC::Ref<SVGLength>> svg_length_from_value(JS::Value value)
{
    if (value.is_object()) {
        if (auto* length = Bindings::impl_from<SVGLength>(&value.as_object()))
            return GC::Ref { *length };
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value must be an SVGLength"sv };
}

WebIDL::ExceptionOr<void> SVGLengthList::set_value_of_new_indexed_property(u32 index, JS::Value value)
{
    TRY(replace_item(TRY(svg_length_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGLengthList::set_value_of_existing_indexed_property(u32 index, JS::Value value)
{
    TRY(replace_item(TRY(svg_length_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGLengthList::set_value_of_indexed_property(u32 index, JS::Value value)
{
    TRY(replace_item(TRY(svg_length_from_value(value)), index));
    return {};
}

void SVGLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
