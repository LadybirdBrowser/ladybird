/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DOMRectReadOnly.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRectReadOnly);

GC::Ref<DOMRectReadOnly> DOMRectReadOnly::create(double x, double y, double width, double height)
{
    return GC::Heap::the().allocate<DOMRectReadOnly>(x, y, width, height);
}

GC::Ref<DOMRectReadOnly> DOMRectReadOnly::create()
{
    return GC::Heap::the().allocate<DOMRectReadOnly>();
}

GC::Ref<DOMRectReadOnly> DOMRectReadOnly::dom_rect_read_only_from_rect(Bindings::DOMRectInit const& other)
{
    return create(other.x, other.y, other.width, other.height);
}

DOMRectReadOnly::DOMRectReadOnly(double x, double y, double width, double height)
    : m_rect(x, y, width, height)
{
}

DOMRectReadOnly::DOMRectReadOnly()
{
}

DOMRectReadOnly::~DOMRectReadOnly() = default;

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMRectReadOnly::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[X]] to value’s x coordinate.
    serialized.encode(x());

    // 2. Set serialized.[[Y]] to value’s y coordinate.
    serialized.encode(y());

    // 3. Set serialized.[[Width]] to value’s width.
    serialized.encode(width());

    // 4. Set serialized.[[Height]] to value’s height.
    serialized.encode(height());

    return {};
}

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMRectReadOnly::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value’s x coordinate to serialized.[[X]].
    auto x = TRY(HTML::decode_or_throw_data_clone_error<double>(realm, serialized));

    // 2. Set value’s y coordinate to serialized.[[Y]].
    auto y = TRY(HTML::decode_or_throw_data_clone_error<double>(realm, serialized));

    // 3. Set value’s width to serialized.[[Width]].
    auto width = TRY(HTML::decode_or_throw_data_clone_error<double>(realm, serialized));

    // 4. Set value’s height to serialized.[[Height]].
    auto height = TRY(HTML::decode_or_throw_data_clone_error<double>(realm, serialized));

    m_rect = Gfx::DoubleRect { x, y, width, height };
    return {};
}

}
