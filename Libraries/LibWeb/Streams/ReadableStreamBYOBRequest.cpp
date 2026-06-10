/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStreamBYOBRequest.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStreamBYOBRequest);

// https://streams.spec.whatwg.org/#rs-byob-request-view
WebIDL::NullableArrayBufferViewVariant ReadableStreamBYOBRequest::view()
{
    // 1. Return this.[[view]].
    if (!m_view.has_value())
        return Empty {};
    return m_view->array_buffer_view();
}

void ReadableStreamBYOBRequest::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_controller);
    if (m_view.has_value()) {
        m_view->array_buffer_view().visit([&](auto const& view) {
            visitor.visit(view);
        });
    }
}

// https://streams.spec.whatwg.org/#rs-byob-request-respond
WebIDL::ExceptionOr<void> ReadableStreamBYOBRequest::respond(JS::Realm& realm, WebIDL::UnsignedLongLong bytes_written)
{
    // 1. If this.[[controller]] is undefined, throw a TypeError exception.
    if (!m_controller)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Controller is undefined"_string };

    // 2. If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a TypeError exception.
    VERIFY(m_view.has_value());
    if (m_view->viewed_array_buffer()->is_detached())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unable to respond to detached ArrayBuffer"_string };

    // 3. Assert: this.[[view]].[[ByteLength]] > 0.
    VERIFY(m_view->viewed_array_buffer()->byte_length() > 0);

    // 4. Assert: this.[[view]].[[ViewedArrayBuffer]].[[ByteLength]] > 0.
    VERIFY(m_view->viewed_array_buffer()->byte_length() > 0);

    // 5. Perform ? ReadableByteStreamControllerRespond(this.[[controller]], bytesWritten).
    return readable_byte_stream_controller_respond(realm, *m_controller, bytes_written);
}

// https://streams.spec.whatwg.org/#rs-byob-request-respond-with-new-view
WebIDL::ExceptionOr<void> ReadableStreamBYOBRequest::respond_with_new_view(JS::Realm& realm, WebIDL::ArrayBufferViewVariant const& view)
{
    WebIDL::ArrayBufferView array_buffer_view { view };

    // 1. If this.[[controller]] is undefined, throw a TypeError exception.
    if (!m_controller)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Controller is undefined"_string };

    // 2. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, throw a TypeError exception.
    if (array_buffer_view.viewed_array_buffer()->is_detached())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unable to respond with a detached ArrayBuffer"_string };

    // 3. Return ? ReadableByteStreamControllerRespondWithNewView(this.[[controller]], view).
    return TRY(readable_byte_stream_controller_respond_with_new_view(realm, *m_controller, array_buffer_view));
}

}
