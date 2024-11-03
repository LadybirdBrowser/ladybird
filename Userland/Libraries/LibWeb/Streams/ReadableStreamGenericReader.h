/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#readablestreamgenericreader
class ReadableStreamGenericReaderMixin {
public:
    virtual ~ReadableStreamGenericReaderMixin() = default;

    JS::GCPtr<WebIDL::Promise> closed();

    JS::NonnullGCPtr<WebIDL::Promise> cancel(JS::Value reason);

    JS::GCPtr<ReadableStream> stream() const { return m_stream; }
    void set_stream(JS::GCPtr<ReadableStream> stream) { m_stream = stream; }

    JS::GCPtr<WebIDL::Promise> closed_promise_capability() { return m_closed_promise; }
    void set_closed_promise_capability(JS::GCPtr<WebIDL::Promise> promise) { m_closed_promise = promise; }

protected:
    explicit ReadableStreamGenericReaderMixin(JS::Realm&);

    void visit_edges(JS::Cell::Visitor&);

    // https://streams.spec.whatwg.org/#readablestreamgenericreader-closedpromise
    // A promise returned by the reader's closed getter
    JS::GCPtr<WebIDL::Promise> m_closed_promise;

    // https://streams.spec.whatwg.org/#readablestreamgenericreader-stream
    // A ReadableStream instance that owns this reader
    JS::GCPtr<ReadableStream> m_stream;

    JS::NonnullGCPtr<JS::Realm> m_realm;
};

}
