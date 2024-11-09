/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/SinglyLinkedList.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#readablestreamdefaultcontroller
class ReadableStreamDefaultController : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ReadableStreamDefaultController, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(ReadableStreamDefaultController);

public:
    explicit ReadableStreamDefaultController(JS::Realm&);
    virtual ~ReadableStreamDefaultController() override = default;

    Optional<double> desired_size();

    WebIDL::ExceptionOr<void> close();
    WebIDL::ExceptionOr<void> enqueue(JS::Value chunk);
    void error(JS::Value error);

    JS::GCPtr<CancelAlgorithm> cancel_algorithm() { return m_cancel_algorithm; }
    void set_cancel_algorithm(JS::GCPtr<CancelAlgorithm> value) { m_cancel_algorithm = value; }

    bool close_requested() const { return m_close_requested; }
    void set_close_requested(bool value) { m_close_requested = value; }

    bool pull_again() const { return m_pull_again; }
    void set_pull_again(bool value) { m_pull_again = value; }

    JS::GCPtr<PullAlgorithm> pull_algorithm() { return m_pull_algorithm; }
    void set_pull_algorithm(JS::GCPtr<PullAlgorithm> value) { m_pull_algorithm = value; }

    bool pulling() const { return m_pulling; }
    void set_pulling(bool value) { m_pulling = value; }

    SinglyLinkedList<ValueWithSize>& queue() { return m_queue; }

    double queue_total_size() const { return m_queue_total_size; }
    void set_queue_total_size(double value) { m_queue_total_size = value; }

    bool started() const { return m_started; }
    void set_started(bool value) { m_started = value; }

    double strategy_hwm() const { return m_strategy_hwm; }
    void set_strategy_hwm(double value) { m_strategy_hwm = value; }

    JS::GCPtr<SizeAlgorithm> strategy_size_algorithm() { return m_strategy_size_algorithm; }
    void set_strategy_size_algorithm(JS::GCPtr<SizeAlgorithm> value) { m_strategy_size_algorithm = value; }

    JS::GCPtr<ReadableStream> stream() { return m_stream; }
    void set_stream(JS::GCPtr<ReadableStream> value) { m_stream = value; }

    JS::NonnullGCPtr<WebIDL::Promise> cancel_steps(JS::Value reason);
    void pull_steps(ReadRequest&);
    void release_steps();

private:
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-cancelalgorithm
    // A promise-returning algorithm, taking one argument (the cancel reason), which communicates a requested cancelation to the underlying source
    JS::GCPtr<CancelAlgorithm> m_cancel_algorithm;

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-closerequested
    // A boolean flag indicating whether the stream has been closed by its underlying source, but still has chunks in its internal queue that have not yet been read
    bool m_close_requested { false };

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pullagain
    // A boolean flag set to true if the stream’s mechanisms requested a call to the underlying source's pull algorithm to pull more data, but the pull could not yet be done since a previous call is still executing
    bool m_pull_again { false };

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pullalgorithm
    // A promise-returning algorithm that pulls data from the underlying source
    JS::GCPtr<PullAlgorithm> m_pull_algorithm;

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pulling
    // A boolean flag set to true while the underlying source's pull algorithm is executing and the returned promise has not yet fulfilled, used to prevent reentrant calls
    bool m_pulling { false };

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-queue
    // A list representing the stream’s internal queue of chunks
    SinglyLinkedList<ValueWithSize> m_queue;

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-queuetotalsize
    // The total size of all the chunks stored in [[queue]]
    double m_queue_total_size { 0 };

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-started
    // A boolean flag indicating whether the underlying source has finished starting
    bool m_started { false };

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-strategyhwm
    // A number supplied to the constructor as part of the stream’s queuing strategy, indicating the point at which the stream will apply backpressure to its underlying source
    double m_strategy_hwm { 0 };

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-strategysizealgorithm
    // An algorithm to calculate the size of enqueued chunks, as part of the stream’s queuing strategy
    JS::GCPtr<SizeAlgorithm> m_strategy_size_algorithm;

    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-stream
    // The ReadableStream instance controlled
    JS::GCPtr<ReadableStream> m_stream;
};

}
