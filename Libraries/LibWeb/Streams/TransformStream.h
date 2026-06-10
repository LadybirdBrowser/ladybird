/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/Algorithms.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

class TransformStream final
    : public Bindings::Wrappable
    , public Bindings::Transferable {
    WEB_WRAPPABLE(TransformStream, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TransformStream);

public:
    virtual ~TransformStream() override;

    static WebIDL::ExceptionOr<GC::Ref<TransformStream>> create_for_constructor(JS::Realm&, GC::Ptr<JS::Object> transformer_object, QueuingStrategy const& writable_strategy = {}, QueuingStrategy const& readable_strategy = {});
    static WebIDL::ExceptionOr<GC::Ref<TransformStream>> create(JS::Realm&, GC::Ptr<JS::Object> transformer_object, Transformer const&, QueuingStrategy const& writable_strategy = {}, QueuingStrategy const& readable_strategy = {});

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
    JS::Realm& backpressure_change_promise_realm() const;

    GC::Ptr<TransformStreamDefaultController> controller() const { return m_controller; }
    void set_controller(GC::Ptr<TransformStreamDefaultController> value) { m_controller = value; }

    void set_up(JS::Realm&, GC::Ref<TransformAlgorithm>, GC::Ptr<FlushAlgorithm> = {}, GC::Ptr<CancelAlgorithm> = {});
    void enqueue(JS::Value chunk);

    // ^Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(JS::Realm&, HTML::TransferDataEncoder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(JS::Realm&, HTML::TransferDataDecoder&) override;
    virtual HTML::TransferType primary_interface() const override { return HTML::TransferType::TransformStream; }

private:
    TransformStream();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#transformstream-backpressure
    // Whether there was backpressure on [[readable]] the last time it was observed
    Optional<bool> m_backpressure { false };

    // https://streams.spec.whatwg.org/#transformstream-backpressurechangepromise
    // A promise which is fulfilled and replaced every time the value of [[backpressure]] changes
    GC::Ptr<WebIDL::Promise> m_backpressure_change_promise;

    // https://streams.spec.whatwg.org/#transformstream-controller
    // A TransformStreamDefaultController created with the ability to control [[readable]] and [[writable]]
    GC::Ptr<TransformStreamDefaultController> m_controller;

    // https://streams.spec.whatwg.org/#transformstream-readable
    // The ReadableStream instance controlled by this object
    GC::Ptr<ReadableStream> m_readable;

    // https://streams.spec.whatwg.org/#transformstream-writable
    // The WritableStream instance controlled by this object
    GC::Ptr<WritableStream> m_writable;
};

}
