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

JS_DEFINE_ALLOCATOR(DOMException);

JS::NonnullGCPtr<DOMException> DOMException::create(JS::Realm& realm, FlyString name, String message)
{
    return realm.heap().allocate<DOMException>(realm, realm, move(name), move(message));
}

JS::NonnullGCPtr<DOMException> DOMException::construct_impl(JS::Realm& realm, String message, FlyString name)
{
    return realm.heap().allocate<DOMException>(realm, realm, move(name), move(message));
}

DOMException::DOMException(JS::Realm& realm, FlyString name, String message)
    : PlatformObject(realm)
    , m_name(move(name))
    , m_message(move(message))
{
}

DOMException::~DOMException() = default;

void DOMException::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMException);
}

}

// https://webidl.spec.whatwg.org/#ref-for-serialization-steps%E2%91%A2
Web::WebIDL::ExceptionOr<void> Web::WebIDL::DOMException::serialization_steps(HTML::SerializationRecord& record, bool, HTML::SerializationMemory&)
{
    auto& vm = this->vm();

    // 1. Set serialized.[[Name]] to value’s name.
    TRY(HTML::serialize_string(vm, record, m_name.to_string()));

    // 2. Set serialized.[[Message]] to value’s message.
    TRY(HTML::serialize_string(vm, record, m_message.to_string()));

    // FIXME: 3. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.

    return {};
}

// https://webidl.spec.whatwg.org/#ref-for-deserialization-steps%E2%91%A2
Web::WebIDL::ExceptionOr<void> Web::WebIDL::DOMException::deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&)
{
    auto& vm = this->vm();

    // 1. Set value’s name to serialized.[[Name]].
    m_name = TRY(HTML::deserialize_string(vm, record, position));

    // 2. Set value’s message to serialized.[[Message]].
    m_message = TRY(HTML::deserialize_string(vm, record, position));

    // FIXME: 3. If any other data is attached to serialized, then deserialize and attach it to value.

    return {};
}
