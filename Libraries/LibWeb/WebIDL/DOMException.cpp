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

GC::Ref<DOMException> DOMException::create(JS::Realm& realm, FlyString name, Utf16String const& message)
{
    return realm.create<DOMException>(realm, move(name), message);
}

GC::Ref<DOMException> DOMException::create(JS::Realm& realm)
{
    return realm.create<DOMException>(realm);
}

GC::Ref<DOMException> DOMException::construct_impl(JS::Realm& realm, Utf16String const& message, FlyString name)
{
    return realm.create<DOMException>(realm, move(name), message);
}

DOMException::DOMException(JS::Realm& realm, FlyString name, Utf16String const& message)
    : PlatformObject(realm)
    , m_name(move(name))
    , m_message(message)
{
}

DOMException::DOMException(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DOMException::~DOMException() = default;

void DOMException::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMException);
    Base::initialize(realm);
}

WebIDL::ExceptionOr<void> DOMException::serialization_steps(HTML::TransferDataEncoder& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[Name]] to value’s name.
    serialized.encode(m_name.to_string());

    // 2. Set serialized.[[Message]] to value’s message.
    serialized.encode(m_message.to_utf16_string());

    // FIXME: 3. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.

    return {};
}

WebIDL::ExceptionOr<void> DOMException::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value’s name to serialized.[[Name]].
    m_name = serialized.decode<String>();

    // 2. Set value’s message to serialized.[[Message]].
    m_message = serialized.decode<Utf16String>();

    // FIXME: 3. If any other data is attached to serialized, then deserialize and attach it to value.

    return {};
}

}
