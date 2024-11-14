/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

class TransformStream final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TransformStream, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TransformStream);

public:
    virtual ~TransformStream() override;

    static WebIDL::ExceptionOr<GC::Ref<TransformStream>> construct_impl(JS::Realm&, Optional<GC::Root<JS::Object>> transformer_object = {}, QueuingStrategy const& writable_strategy = {}, QueuingStrategy const& readable_strategy = {});

    // https://streams.spec.whatwg.org/#ts-readable
    GC::Ref<ReadableStream> readable() { return *m_readable; }
    void set_readable(ReadableStream& readable) { m_readable = readable; }

    // https://streams.spec.whatwg.org/#ts-writable
    GC::Ref<WritableStream> writable() { return *m_writable; }
    void set_writable(WritableStream& writable) { m_writable = writable; }

    Optional<bool> backpressure() const { return m_backpressure; }
    void set_backpressure(Optional<bool> value) { m_backpressure = move(value); }

    GC::Ptr<WebIDL::Promise> backpressure_change_promise() const { return m_backpressure_change_promise; }
    void set_backpressure_change_promise(GC::Ptr<WebIDL::Promise> value) { m_backpressure_change_promise = value; }

    GC::Ptr<TransformStreamDefaultController> controller() const { return m_controller; }
    void set_controller(GC::Ptr<TransformStreamDefaultController> value) { m_controller = value; }

private:
    explicit TransformStream(JS::Realm& realm);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#transformstream-backpressure
    // Whether there was backpressure on [[readable]] the last time it was observed
    Optional<bool> m_backpressure { false };

    // https://streams.spec.whatwg.org/#transformstream-backpressurechangepromise
    // A promise which is fulfilled and replaced every time the value of [[backpressure]] changes
    GC::Ptr<WebIDL::Promise> m_backpressure_change_promise;

    // https://streams.spec.whatwg.org/#transformstream-controller
    // A TransformStreamDefaultController created with the ability to control [[readable]] and [[writable]]
    GC::Ptr<TransformStreamDefaultController> m_controller;

    // https://streams.spec.whatwg.org/#transformstream-detached
    // A boolean flag set to true when the stream is transferred
    bool m_detached { false };

    // https://streams.spec.whatwg.org/#transformstream-readable
    // The ReadableStream instance controlled by this object
    GC::Ptr<ReadableStream> m_readable;

    // https://streams.spec.whatwg.org/#transformstream-writable
    // The WritableStream instance controlled by this object
    GC::Ptr<WritableStream> m_writable;
};

}
