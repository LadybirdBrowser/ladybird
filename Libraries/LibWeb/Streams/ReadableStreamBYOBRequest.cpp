/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamBYOBRequestPrototype.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStreamBYOBRequest.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Streams {

JS_DEFINE_ALLOCATOR(ReadableStreamBYOBRequest);

// https://streams.spec.whatwg.org/#rs-byob-request-view
JS::GCPtr<WebIDL::ArrayBufferView> ReadableStreamBYOBRequest::view()
{
    // 1. Return this.[[view]].
    return m_view;
}

ReadableStreamBYOBRequest::ReadableStreamBYOBRequest(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void ReadableStreamBYOBRequest::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStreamBYOBRequest);
}

void ReadableStreamBYOBRequest::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_controller);
    visitor.visit(m_view);
}

// https://streams.spec.whatwg.org/#rs-byob-request-respond
WebIDL::ExceptionOr<void> ReadableStreamBYOBRequest::respond(WebIDL::UnsignedLongLong bytes_written)
{
    // 1. If this.[[controller]] is undefined, throw a TypeError exception.
    if (!m_controller)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Controller is undefined"_string };

    // 2. If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a TypeError exception.
    if (m_view->viewed_array_buffer()->is_detached())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unable to respond to detached ArrayBuffer"_string };

    // 3. Assert: this.[[view]].[[ByteLength]] > 0.
    VERIFY(m_view->viewed_array_buffer()->byte_length() > 0);

    // 4. Assert: this.[[view]].[[ViewedArrayBuffer]].[[ByteLength]] > 0.
    VERIFY(m_view->viewed_array_buffer()->byte_length() > 0);

    // 5. Perform ? ReadableByteStreamControllerRespond(this.[[controller]], bytesWritten).
    return readable_byte_stream_controller_respond(*m_controller, bytes_written);
}

// https://streams.spec.whatwg.org/#rs-byob-request-respond-with-new-view
WebIDL::ExceptionOr<void> ReadableStreamBYOBRequest::respond_with_new_view(JS::Handle<WebIDL::ArrayBufferView> const& view)
{
    auto& realm = this->realm();

    // 1. If this.[[controller]] is undefined, throw a TypeError exception.
    if (!m_controller)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Controller is undefined"_string };

    // 2. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, throw a TypeError exception.
    if (view->viewed_array_buffer()->is_detached())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unable to respond with a detached ArrayBuffer"_string };

    // 3. Return ? ReadableByteStreamControllerRespondWithNewView(this.[[controller]], view).
    return TRY(readable_byte_stream_controller_respond_with_new_view(realm, *m_controller, *view));
}

}
