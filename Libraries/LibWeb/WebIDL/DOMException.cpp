/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
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
    : m_error_data(JS::ErrorDataCell::capture(JS::VM::the()))
    , m_name(Utf16FlyString::from_fly_string(name))
    , m_message(message)
{
}

DOMException::DOMException(Utf16FlyString name, Utf16String const& message)
    : m_error_data(JS::ErrorDataCell::capture(JS::VM::the()))
    , m_name(move(name))
    , m_message(message)
{
}

DOMException::DOMException()
    : m_error_data(JS::ErrorDataCell::capture(JS::VM::the()))
{
}

DOMException::~DOMException() = default;

void DOMException::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error_data);
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

// What the serialization steps of DOMException, which run in Rust (Libraries/LibWeb/Bindings/Rust/src/webidl), read of a
// DOMException and create one with. Rust owns the strings that it reads, and lends those that it creates one with.
extern "C" {

void web_dom_exception_name_and_message(JSObject*, FlatPtr* name, FlatPtr* message);
JSObject* web_dom_exception_create(void* realm, FlatPtr name, FlatPtr message);
}

void web_dom_exception_name_and_message(JSObject* object, FlatPtr* name, FlatPtr* message)
{
    auto& exception = Web::Bindings::impl_from<Web::WebIDL::DOMException>(*JS::HostABI::object_from_abi(object));
    *name = exception.name().to_utf16_string().into_raw();
    *message = exception.message().to_utf16_string().into_raw();
}

JSObject* web_dom_exception_create(void* realm, FlatPtr name, FlatPtr message)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto name_string = Utf16String::from_raw(name);
    auto exception = Web::WebIDL::DOMException::create(Utf16FlyString::from_utf16(name_string.utf16_view()), Utf16String::from_raw(message));
    return JS::HostABI::object_to_abi(Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(js_realm), js_realm, exception).ptr());
}
