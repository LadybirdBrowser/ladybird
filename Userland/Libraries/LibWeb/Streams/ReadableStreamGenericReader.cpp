/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamGenericReader.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#generic-reader-closed
GC::Ptr<JS::Promise> ReadableStreamGenericReaderMixin::closed()
{
    // 1. Return this.[[closedPromise]].
    return GC::Ptr { verify_cast<JS::Promise>(*m_closed_promise->promise()) };
}

// https://streams.spec.whatwg.org/#generic-reader-cancel
GC::Ref<JS::Promise> ReadableStreamGenericReaderMixin::cancel(JS::Value reason)
{
    // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
    if (!m_stream) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "No stream present to cancel"sv };
        return WebIDL::create_rejected_promise_from_exception(m_realm, move(exception));
    }

    // 2. Return ! ReadableStreamReaderGenericCancel(this, reason).
    auto promise_capability = readable_stream_reader_generic_cancel(*this, reason);
    return GC::Ref { verify_cast<JS::Promise>(*promise_capability->promise().ptr()) };
}

ReadableStreamGenericReaderMixin::ReadableStreamGenericReaderMixin(JS::Realm& realm)
    : m_realm(realm)
{
}

void ReadableStreamGenericReaderMixin::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_closed_promise);
    visitor.visit(m_stream);
    visitor.visit(m_realm);
}

}
