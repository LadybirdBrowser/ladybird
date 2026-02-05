/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Forward.h>

namespace Web::Streams::Detail {

// https://streams.spec.whatwg.org/#ref-for-in-parallel
class ReadableStreamPipeTo final : public JS::Cell {
    GC_CELL(ReadableStreamPipeTo, JS::Cell);
    GC_DECLARE_ALLOCATOR(ReadableStreamPipeTo);

public:
    void process();

    void set_abort_signal(GC::Ref<DOM::AbortSignal>, DOM::AbortSignal::AbortSignal::AbortAlgorithmID);

    void shutdown_with_action(GC::Ref<GC::Function<GC::Ref<WebIDL::Promise>()>> action, Optional<JS::Value> original_error = {});
    void shutdown(Optional<JS::Value> error = {});

private:
    ReadableStreamPipeTo(
        GC::Ref<JS::Realm>,
        GC::Ref<WebIDL::Promise>,
        GC::Ref<ReadableStream> source,
        GC::Ref<WritableStream> destination,
        GC::Ref<ReadableStreamDefaultReader> reader,
        GC::Ref<WritableStreamDefaultWriter> writer,
        bool prevent_close,
        bool prevent_abort,
        bool prevent_cancel);

    virtual void visit_edges(Cell::Visitor& visitor) override;

    void read_chunk();
    void write_chunk();

    void write_unwritten_chunks();
    void wait_for_pending_writes_to_complete(Function<void()> on_complete);

    void finish(Optional<JS::Value> error = {});

    bool check_for_error_and_close_states();
    bool check_for_forward_errors();
    bool check_for_backward_errors();
    bool check_for_forward_close();
    bool check_for_backward_close();

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<WebIDL::Promise> m_promise;

    GC::Ref<ReadableStream> m_source;
    GC::Ref<WritableStream> m_destination;

    GC::Ref<ReadableStreamDefaultReader> m_reader;
    GC::Ref<WritableStreamDefaultWriter> m_writer;

    GC::Ptr<DOM::AbortSignal> m_signal;
    DOM::AbortSignal::AbortAlgorithmID m_signal_id { 0 };

    GC::Ptr<WebIDL::Promise> m_last_write_promise;
    Vector<JS::Value, 1> m_unwritten_chunks;

    GC::Ref<WebIDL::ReactionSteps> m_on_shutdown;

    bool m_prevent_close { false };
    bool m_prevent_abort { false };
    bool m_prevent_cancel { false };

    bool m_shutting_down { false };
};

}
