/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(DOMException);

GC::Ref<DOMException> DOMException::create(FlyString name, Utf16String const& message)
{
    return GC::Heap::the().allocate<DOMException>(move(name), message);
}

GC::Ref<DOMException> DOMException::create(Utf16FlyString name, Utf16String const& message)
{
    return GC::Heap::the().allocate<DOMException>(move(name), message);
}

GC::Ref<DOMException> DOMException::create()
{
    return GC::Heap::the().allocate<DOMException>();
}

DOMException::DOMException(FlyString name, Utf16String const& message)
    : ErrorData(JS::VM::the())
    , m_name(Utf16FlyString::from_fly_string(name))
    , m_message(message)
{
}

DOMException::DOMException(Utf16FlyString name, Utf16String const& message)
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

WebIDL::ExceptionOr<void> DOMException::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[Name]] to value’s name.
    serialized.encode(m_name.to_utf16_string());

    // 2. Set serialized.[[Message]] to value’s message.
    serialized.encode(m_message.to_utf16_string());

    // FIXME: 3. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.

    return {};
}

WebIDL::ExceptionOr<void> DOMException::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value’s name to serialized.[[Name]].
    m_name = TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized));

    // 2. Set value’s message to serialized.[[Message]].
    m_message = TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized));

    // FIXME: 3. If any other data is attached to serialized, then deserialize and attach it to value.

    return {};
}

}

namespace Web::Bindings {

WebIDL::DOMException* dom_exception_from_object(JS::Object& object)
{
    return Bindings::impl_from<WebIDL::DOMException>(&object);
}

Optional<DOMExceptionReportDetails> dom_exception_report_details(JS::Object& object)
{
    auto const* exception = dom_exception_from_object(object);
    if (!exception)
        return {};
    return DOMExceptionReportDetails {
        .name = MUST(exception->name().view().to_utf8()),
        .message = MUST(exception->message().view().to_utf8()),
    };
}

}

namespace Web {

JS::Completion throw_completion(JS::Realm& realm, GC::Ref<WebIDL::DOMException> exception)
{
    return JS::throw_completion(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, exception));
}

}
