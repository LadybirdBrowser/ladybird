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
    GC_DECLARE_ALLOCATOR(WritableStreamDefaultWriter);

public:
    static WebIDL::ExceptionOr<GC::Ref<WritableStreamDefaultWriter>> construct_impl(JS::Realm&, GC::Ref<WritableStream>);

    virtual ~WritableStreamDefaultWriter() override = default;

    GC::Ptr<WebIDL::Promise> closed();
    WebIDL::ExceptionOr<Optional<double>> desired_size() const;
    GC::Ptr<WebIDL::Promise> ready();
    GC::Ref<WebIDL::Promise> abort(JS::Value reason);
    GC::Ref<WebIDL::Promise> close();
    void release_lock();
    GC::Ref<WebIDL::Promise> write(JS::Value chunk);

    GC::Ptr<WebIDL::Promise> closed_promise() { return m_closed_promise; }
    void set_closed_promise(GC::Ptr<WebIDL::Promise> value) { m_closed_promise = value; }

    GC::Ptr<WebIDL::Promise> ready_promise() { return m_ready_promise; }
    void set_ready_promise(GC::Ptr<WebIDL::Promise> value) { m_ready_promise = value; }

    GC::Ptr<WritableStream const> stream() const { return m_stream; }
    GC::Ptr<WritableStream> stream() { return m_stream; }
    void set_stream(GC::Ptr<WritableStream> value) { m_stream = value; }

private:
    explicit WritableStreamDefaultWriter(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-closedpromise
    // A promise returned by the writer’s closed getter
    GC::Ptr<WebIDL::Promise> m_closed_promise;

    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-readypromise
    // A promise returned by the writer’s ready getter
    GC::Ptr<WebIDL::Promise> m_ready_promise;

    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-stream
    // A WritableStream instance that owns this reader
    GC::Ptr<WritableStream> m_stream;
};

}
