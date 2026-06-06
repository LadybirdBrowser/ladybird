/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLengthList);

GC::Ref<SVGLengthList> SVGLengthList::create(Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGLengthList>(move(items), read_only);
}

GC::Ref<SVGLengthList> SVGLengthList::create(ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGLengthList>(read_only);
}

SVGLengthList::SVGLengthList(Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
    : SVGList(move(items), read_only)
{
}

SVGLengthList::SVGLengthList(ReadOnlyList read_only)
    : SVGList(read_only)
{
}

Optional<JS::Value> SVGLengthList::item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const
{
    if (index >= items().size())
        return {};
    return Bindings::wrap(wrapper_world, realm, items()[index]);
}

static WebIDL::ExceptionOr<GC::Ref<SVGLength>> svg_length_from_value(JS::Value value)
{
    if (value.is_object()) {
        if (auto* length = Bindings::impl_from<SVGLength>(&value.as_object()))
            return GC::Ref { *length };
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value must be an SVGLength"sv };
}

WebIDL::ExceptionOr<void> SVGLengthList::set_value_of_new_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_length_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGLengthList::set_value_of_existing_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_length_from_value(value)), index));
    return {};
}

WebIDL::ExceptionOr<void> SVGLengthList::set_value_of_indexed_property(JS::Realm& realm, u32 index, JS::Value value)
{
    TRY(replace_item(realm, TRY(svg_length_from_value(value)), index));
    return {};
}

void SVGLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
