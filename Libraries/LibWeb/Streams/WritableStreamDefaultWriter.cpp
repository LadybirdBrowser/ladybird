/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WritableStreamDefaultWriterPrototype.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultWriter.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(WritableStreamDefaultWriter);

WebIDL::ExceptionOr<GC::Ref<WritableStreamDefaultWriter>> WritableStreamDefaultWriter::construct_impl(JS::Realm& realm, GC::Ref<WritableStream> stream)
{
    auto writer = realm.create<WritableStreamDefaultWriter>(realm);

    // 1. Perform ? SetUpWritableStreamDefaultWriter(this, stream).
    TRY(set_up_writable_stream_default_writer(*writer, stream));

    return writer;
}

// https://streams.spec.whatwg.org/#default-writer-closed
GC::Ptr<WebIDL::Promise> WritableStreamDefaultWriter::closed()
{
    // 1. Return this.[[closedPromise]].
    return m_closed_promise;
}

// https://streams.spec.whatwg.org/#default-writer-desired-size
WebIDL::ExceptionOr<Optional<double>> WritableStreamDefaultWriter::desired_size() const
{
    // 1. If this.[[stream]] is undefined, throw a TypeError exception.
    if (!m_stream)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot get desired size of writer that has no locked stream"sv };

    // 2. Return ! WritableStreamDefaultWriterGetDesiredSize(this).
    return writable_stream_default_writer_get_desired_size(*this);
}

// https://streams.spec.whatwg.org/#default-writer-ready
GC::Ptr<WebIDL::Promise> WritableStreamDefaultWriter::ready()
{
    // 1. Return this.[[readyPromise]].
    return m_ready_promise;
}

// https://streams.spec.whatwg.org/#default-writer-abort
GC::Ref<WebIDL::Promise> WritableStreamDefaultWriter::abort(JS::Value reason)
{
    auto& realm = this->realm();

    // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
    if (!m_stream) {
        auto exception = JS::TypeError::create(realm, "Cannot abort a writer that has no locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2. Return ! WritableStreamDefaultWriterAbort(this, reason).
    return writable_stream_default_writer_abort(*this, reason);
}

// https://streams.spec.whatwg.org/#default-writer-close
GC::Ref<WebIDL::Promise> WritableStreamDefaultWriter::close()
{
    auto& realm = this->realm();

    // 1. Let stream be this.[[stream]].

    // 2. If stream is undefined, return a promise rejected with a TypeError exception.
    if (!m_stream) {
        auto exception = JS::TypeError::create(realm, "Cannot close a writer that has no locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 3. If ! WritableStreamCloseQueuedOrInFlight(stream) is true, return a promise rejected with a TypeError exception.
    if (writable_stream_close_queued_or_in_flight(*m_stream)) {
        auto exception = JS::TypeError::create(realm, "Cannot close a stream that is already closed or errored"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 4. Return ! WritableStreamDefaultWriterClose(this).
    return writable_stream_default_writer_close(*this);
}

// https://streams.spec.whatwg.org/#default-writer-release-lock
void WritableStreamDefaultWriter::release_lock()
{
    // 1. Let stream be this.[[stream]].

    // 2. If stream is undefined, return.
    if (!m_stream)
        return;

    // 3. Assert: stream.[[writer]] is not undefined.
    VERIFY(m_stream->writer());

    // 4. Perform ! WritableStreamDefaultWriterRelease(this).
    writable_stream_default_writer_release(*this);
}

// https://streams.spec.whatwg.org/#default-writer-write
GC::Ref<WebIDL::Promise> WritableStreamDefaultWriter::write(JS::Value chunk)
{
    auto& realm = this->realm();

    // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
    if (!m_stream) {
        auto exception = JS::TypeError::create(realm, "Cannot write to a writer that has no locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2. Return ! WritableStreamDefaultWriterWrite(this, chunk).
    return writable_stream_default_writer_write(*this, chunk);
}

WritableStreamDefaultWriter::WritableStreamDefaultWriter(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void WritableStreamDefaultWriter::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WritableStreamDefaultWriter);
}

void WritableStreamDefaultWriter::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_closed_promise);
    visitor.visit(m_ready_promise);
    visitor.visit(m_stream);
}

}
