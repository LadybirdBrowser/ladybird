/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMRectReadOnlyPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRectReadOnly);

WebIDL::ExceptionOr<GC::Ref<DOMRectReadOnly>> DOMRectReadOnly::construct_impl(JS::Realm& realm, double x, double y, double width, double height)
{
    return realm.create<DOMRectReadOnly>(realm, x, y, width, height);
}

// https://drafts.fxtf.org/geometry/#create-a-domrect-from-the-dictionary
GC::Ref<DOMRectReadOnly> DOMRectReadOnly::from_rect(JS::VM& vm, Geometry::DOMRectInit const& other)
{
    auto& realm = *vm.current_realm();
    return realm.create<DOMRectReadOnly>(realm, other.x, other.y, other.width, other.height);
}

GC::Ref<DOMRectReadOnly> DOMRectReadOnly::create(JS::Realm& realm)
{
    return realm.create<DOMRectReadOnly>(realm);
}

DOMRectReadOnly::DOMRectReadOnly(JS::Realm& realm, double x, double y, double width, double height)
    : PlatformObject(realm)
    , m_rect(x, y, width, height)
{
}

DOMRectReadOnly::DOMRectReadOnly(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DOMRectReadOnly::~DOMRectReadOnly() = default;

void DOMRectReadOnly::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMRectReadOnly);
    Base::initialize(realm);
}

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMRectReadOnly::serialization_steps(HTML::TransferDataEncoder& serialized, bool, HTML::SerializationMemory&)
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
WebIDL::ExceptionOr<void> DOMRectReadOnly::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value’s x coordinate to serialized.[[X]].
    auto x = serialized.decode<double>();

    // 2. Set value’s y coordinate to serialized.[[Y]].
    auto y = serialized.decode<double>();

    // 3. Set value’s width to serialized.[[Width]].
    auto width = serialized.decode<double>();

    // 4. Set value’s height to serialized.[[Height]].
    auto height = serialized.decode<double>();

    m_rect = { x, y, width, height };
    return {};
}

}
