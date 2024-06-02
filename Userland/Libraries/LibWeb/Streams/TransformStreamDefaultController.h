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
    JS_DECLARE_ALLOCATOR(TransformStreamDefaultController);

public:
    explicit TransformStreamDefaultController(JS::Realm&);
    virtual ~TransformStreamDefaultController() override;

    Optional<double> desired_size();
    WebIDL::ExceptionOr<void> enqueue(Optional<JS::Value> chunk);
    void error(Optional<JS::Value> reason = {});
    void terminate();

    JS::GCPtr<CancelAlgorithm> cancel_algorithm() { return m_cancel_algorithm; }
    void set_cancel_algorithm(JS::GCPtr<CancelAlgorithm> value) { m_cancel_algorithm = value; }

    JS::GCPtr<JS::PromiseCapability> finish_promise() { return m_finish_promise; }
    void set_finish_promise(JS::GCPtr<JS::PromiseCapability> value) { m_finish_promise = value; }

    JS::GCPtr<FlushAlgorithm> flush_algorithm() { return m_flush_algorithm; }
    void set_flush_algorithm(JS::GCPtr<FlushAlgorithm>&& value) { m_flush_algorithm = move(value); }

    JS::GCPtr<TransformAlgorithm> transform_algorithm() { return m_transform_algorithm; }
    void set_transform_algorithm(JS::GCPtr<TransformAlgorithm>&& value) { m_transform_algorithm = move(value); }

    JS::GCPtr<TransformStream> stream() { return m_stream; }
    void set_stream(JS::GCPtr<TransformStream> stream) { m_stream = stream; }

private:
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-cancelalgorithm
    JS::GCPtr<CancelAlgorithm> m_cancel_algorithm;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-finishpromise
    JS::GCPtr<JS::PromiseCapability> m_finish_promise;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-flushalgorithm
    JS::GCPtr<FlushAlgorithm> m_flush_algorithm;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-transformalgorithm
    JS::GCPtr<TransformAlgorithm> m_transform_algorithm;

    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-stream
    JS::GCPtr<TransformStream> m_stream;
};

}
