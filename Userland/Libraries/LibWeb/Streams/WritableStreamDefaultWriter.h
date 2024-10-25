/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/SinglyLinkedList.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#writablestreamdefaultwriter
class WritableStreamDefaultWriter final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WritableStreamDefaultWriter, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(WritableStreamDefaultWriter);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<WritableStreamDefaultWriter>> construct_impl(JS::Realm&, JS::NonnullGCPtr<WritableStream>);

    virtual ~WritableStreamDefaultWriter() override = default;

    JS::GCPtr<WebIDL::Promise> closed();
    WebIDL::ExceptionOr<Optional<double>> desired_size() const;
    JS::GCPtr<WebIDL::Promise> ready();
    JS::GCPtr<WebIDL::Promise> abort(JS::Value reason);
    JS::GCPtr<WebIDL::Promise> close();
    void release_lock();
    JS::GCPtr<WebIDL::Promise> write(JS::Value chunk);

    JS::GCPtr<WebIDL::Promise> closed_promise() { return m_closed_promise; }
    void set_closed_promise(JS::GCPtr<WebIDL::Promise> value) { m_closed_promise = value; }

    JS::GCPtr<WebIDL::Promise> ready_promise() { return m_ready_promise; }
    void set_ready_promise(JS::GCPtr<WebIDL::Promise> value) { m_ready_promise = value; }

    JS::GCPtr<WritableStream const> stream() const { return m_stream; }
    JS::GCPtr<WritableStream> stream() { return m_stream; }
    void set_stream(JS::GCPtr<WritableStream> value) { m_stream = value; }

private:
    explicit WritableStreamDefaultWriter(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-closedpromise
    // A promise returned by the writer’s closed getter
    JS::GCPtr<WebIDL::Promise> m_closed_promise;

    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-readypromise
    // A promise returned by the writer’s ready getter
    JS::GCPtr<WebIDL::Promise> m_ready_promise;

    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-stream
    // A WritableStream instance that owns this reader
    JS::GCPtr<WritableStream> m_stream;
};

}
