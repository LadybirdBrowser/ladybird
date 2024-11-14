/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Streams/AbstractOperations.h>

namespace Web::Streams {

class TransformStreamDefaultController : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TransformStreamDefaultController, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TransformStreamDefaultController);

public:
    explicit TransformStreamDefaultController(JS::Realm&);
    virtual ~TransformStreamDefaultController() override;

    Optional<double> desired_size();
    WebIDL::ExceptionOr<void> enqueue(Optional<JS::Value> chunk);
    void error(Optional<JS::Value> reason = {});
    void terminate();

    GC::Ptr<CancelAlgorithm> cancel_algorithm() { return m_cancel_algorithm; }
    void set_cancel_algorithm(GC::Ptr<CancelAlgorithm> value) { m_cancel_algorithm = value; }

    GC::Ptr<JS::PromiseCapability> finish_promise() { return m_finish_promise; }
    void set_finish_promise(GC::Ptr<JS::PromiseCapability> value) { m_finish_promise = value; }

    GC::Ptr<FlushAlgorithm> flush_algorithm() { return m_flush_algorithm; }
    void set_flush_algorithm(GC::Ptr<FlushAlgorithm>&& value) { m_flush_algorithm = move(value); }

    GC::Ptr<TransformAlgorithm> transform_algorithm() { return m_transform_algorithm; }
    void set_transform_algorithm(GC::Ptr<TransformAlgorithm>&& value) { m_transform_algorithm = move(value); }

    GC::Ptr<TransformStream> stream() { return m_stream; }
    void set_stream(GC::Ptr<TransformStream> stream) { m_stream = stream; }

private:
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-cancelalgorithm
    GC::Ptr<CancelAlgorithm> m_cancel_algorithm;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-finishpromise
    GC::Ptr<JS::PromiseCapability> m_finish_promise;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-flushalgorithm
    GC::Ptr<FlushAlgorithm> m_flush_algorithm;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-transformalgorithm
    GC::Ptr<TransformAlgorithm> m_transform_algorithm;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-stream
    GC::Ptr<TransformStream> m_stream;
};

}
