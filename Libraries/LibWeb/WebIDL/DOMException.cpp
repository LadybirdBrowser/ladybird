/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(DOMException);

GC::Ref<DOMException> DOMException::create(FlyString name, Utf16String const& message)
{
    return GC::Heap::the().allocate<DOMException>(move(name), message);
}

GC::Ref<DOMException> DOMException::create()
{
    return GC::Heap::the().allocate<DOMException>();
}

GC::Ref<DOMException> DOMException::create(JS::Realm&, FlyString name, Utf16String const& message)
{
    return create(move(name), message);
}

GC::Ref<DOMException> DOMException::create(JS::Realm&)
{
    return create();
}

GC::Ref<DOMException> DOMException::construct_impl(JS::Realm&, Utf16String const& message, FlyString name)
{
    return create(move(name), message);
}

DOMException::DOMException(FlyString name, Utf16String const& message)
    : ErrorData(JS::VM::the())
    , m_name(move(name))
    , m_message(message)
{
}

DOMException::DOMException()
    : ErrorData(JS::VM::the())
{
}

DOMException::~DOMException() = default;

void DOMException::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    ErrorData::visit_edges(visitor);
}

WebIDL::ExceptionOr<void> DOMException::serialization_steps(JS::Realm&, HTML::TransferDataEncoder& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[Name]] to value’s name.
    serialized.encode(m_name.to_string());

    // 2. Set serialized.[[Message]] to value’s message.
    serialized.encode(m_message.to_utf16_string());

    // FIXME: 3. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.

    return {};
}

WebIDL::ExceptionOr<void> DOMException::deserialization_steps(JS::Realm&, HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value’s name to serialized.[[Name]].
    m_name = serialized.decode<String>();

    // 2. Set value’s message to serialized.[[Message]].
    m_message = serialized.decode<Utf16String>();

    // FIXME: 3. If any other data is attached to serialized, then deserialize and attach it to value.

    return {};
}

}

namespace Web {

JS::Completion throw_completion(JS::Realm& realm, GC::Ref<WebIDL::DOMException> exception)
{
    return JS::throw_completion(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, exception));
}

}
