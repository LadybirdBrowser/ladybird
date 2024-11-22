/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMExceptionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(DOMException);

GC::Ref<DOMException> DOMException::create(JS::Realm& realm, FlyString name, String message)
{
    return realm.create<DOMException>(realm, move(name), move(message));
}

GC::Ref<DOMException> DOMException::create(JS::Realm& realm)
{
    return realm.create<DOMException>(realm);
}

GC::Ref<DOMException> DOMException::construct_impl(JS::Realm& realm, String message, FlyString name)
{
    return realm.create<DOMException>(realm, move(name), move(message));
}

DOMException::DOMException(JS::Realm& realm, FlyString name, String message)
    : PlatformObject(realm)
    , m_name(move(name))
    , m_message(move(message))
{
}

DOMException::DOMException(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DOMException::~DOMException() = default;

void DOMException::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMException);
}

ExceptionOr<void> DOMException::serialization_steps(HTML::SerializationRecord& record, bool, HTML::SerializationMemory&)
{
    auto& vm = this->vm();

    // 1. Set serialized.[[Name]] to value’s name.
    TRY(HTML::serialize_string(vm, record, m_name.to_string()));

    // 2. Set serialized.[[Message]] to value’s message.
    TRY(HTML::serialize_string(vm, record, m_message.to_string()));

    // FIXME: 3. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.

    return {};
}

ExceptionOr<void> DOMException::deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&)
{
    auto& vm = this->vm();

    // 1. Set value’s name to serialized.[[Name]].
    m_name = TRY(HTML::deserialize_string(vm, record, position));

    // 2. Set value’s message to serialized.[[Message]].
    m_message = TRY(HTML::deserialize_string(vm, record, position));

    // FIXME: 3. If any other data is attached to serialized, then deserialize and attach it to value.

    return {};
}

}
