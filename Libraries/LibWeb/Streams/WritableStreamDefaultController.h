/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/SinglyLinkedList.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Streams/AbstractOperations.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#writablestreamdefaultcontroller
class WritableStreamDefaultController final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WritableStreamDefaultController, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WritableStreamDefaultController);

public:
    virtual ~WritableStreamDefaultController() override = default;

    void error(JS::Value error);
    GC::Ref<DOM::AbortSignal> signal() { return *m_signal; }
    void set_signal(GC::Ref<DOM::AbortSignal> value) { m_signal = value; }

    GC::Ptr<AbortAlgorithm> abort_algorithm() { return m_abort_algorithm; }
    void set_abort_algorithm(GC::Ptr<AbortAlgorithm> value) { m_abort_algorithm = value; }

    GC::Ptr<CloseAlgorithm> close_algorithm() { return m_close_algorithm; }
    void set_close_algorithm(GC::Ptr<CloseAlgorithm> value) { m_close_algorithm = value; }

    SinglyLinkedList<ValueWithSize>& queue() { return m_queue; }

    double queue_total_size() const { return m_queue_total_size; }
    void set_queue_total_size(double value) { m_queue_total_size = value; }

    bool started() const { return m_started; }
    void set_started(bool value) { m_started = value; }

    size_t strategy_hwm() const { return m_strategy_hwm; }
    void set_strategy_hwm(size_t value) { m_strategy_hwm = value; }

    GC::Ptr<SizeAlgorithm> strategy_size_algorithm() { return m_strategy_size_algorithm; }
    void set_strategy_size_algorithm(GC::Ptr<SizeAlgorithm> value) { m_strategy_size_algorithm = value; }

    GC::Ref<WritableStream> stream() { return *m_stream; }
    void set_stream(GC::Ref<WritableStream> value) { m_stream = value; }

    GC::Ptr<WriteAlgorithm> write_algorithm() { return m_write_algorithm; }
    void set_write_algorithm(GC::Ptr<WriteAlgorithm> value) { m_write_algorithm = value; }

    GC::Ref<WebIDL::Promise> abort_steps(JS::Value reason);
    void error_steps();

private:
    explicit WritableStreamDefaultController(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Visitor&) override;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-abortalgorithm
    // A promise-returning algorithm, taking one argument (the abort reason), which communicates a requested abort to the underlying sink
    GC::Ptr<AbortAlgorithm> m_abort_algorithm;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-closealgorithm
    // A promise-returning algorithm which communicates a requested close to the underlying sink
    GC::Ptr<CloseAlgorithm> m_close_algorithm;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-queue
    // A list representing the stream’s internal queue of chunks
    SinglyLinkedList<ValueWithSize> m_queue;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-queuetotalsize
    // The total size of all the chunks stored in [[queue]]
    double m_queue_total_size { 0 };

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-signal
    // An AbortSignal that can be used to abort the pending write or close operation when the stream is aborted.
    GC::Ptr<DOM::AbortSignal> m_signal;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-started
    // A boolean flag indicating whether the underlying sink has finished starting
    bool m_started { false };

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-strategyhwm
    // A number supplied by the creator of the stream as part of the stream’s queuing strategy, indicating the point at which the stream will apply backpressure to its underlying sink
    size_t m_strategy_hwm { 0 };

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-strategysizealgorithm
    // An algorithm to calculate the size of enqueued chunks, as part of the stream’s queuing strategy
    GC::Ptr<SizeAlgorithm> m_strategy_size_algorithm;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-stream
    // The WritableStream instance controlled
    GC::Ptr<WritableStream> m_stream;

    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-writealgorithm
    // A promise-returning algorithm, taking one argument (the chunk to write), which writes data to the underlying sink
    GC::Ptr<WriteAlgorithm> m_write_algorithm;
};

}
