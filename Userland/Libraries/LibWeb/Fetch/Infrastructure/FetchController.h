/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-controller
class FetchController : public JS::Cell {
    JS_CELL(FetchController, JS::Cell);
    JS_DECLARE_ALLOCATOR(FetchController);

public:
    enum class State {
        Ongoing,
        Terminated,
        Aborted,
    };

    [[nodiscard]] static JS::NonnullGCPtr<FetchController> create(JS::VM&);

    void set_full_timing_info(JS::NonnullGCPtr<FetchTimingInfo> full_timing_info) { m_full_timing_info = full_timing_info; }
    void set_report_timing_steps(Function<void(JS::Object const&)> report_timing_steps);
    void set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps);

    [[nodiscard]] State state() const { return m_state; }

    void report_timing(JS::Object const&) const;
    void process_next_manual_redirect() const;
    [[nodiscard]] JS::NonnullGCPtr<FetchTimingInfo> extract_full_timing_info() const;
    void abort(JS::Realm&, Optional<JS::Value>);
    void terminate();

    void set_fetch_params(Badge<FetchParams>, JS::NonnullGCPtr<FetchParams> fetch_params) { m_fetch_params = fetch_params; }

    void stop_fetch();

    u64 next_fetch_task_id() { return m_next_fetch_task_id++; }
    void fetch_task_queued(u64 fetch_task_id, HTML::TaskID event_id);
    void fetch_task_complete(u64 fetch_task_id);

private:
    FetchController();

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#fetch-controller-state
    // state (default "ongoing")
    //    "ongoing", "terminated", or "aborted"
    State m_state { State::Ongoing };

    // https://fetch.spec.whatwg.org/#fetch-controller-full-timing-info
    // full timing info (default null)
    //    Null or a fetch timing info.
    JS::GCPtr<FetchTimingInfo> m_full_timing_info;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // report timing steps (default null)
    //    Null or an algorithm accepting a global object.
    JS::GCPtr<JS::HeapFunction<void(JS::Object const&)>> m_report_timing_steps;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // FIXME: serialized abort reason (default null)
    //     Null or a Record (result of StructuredSerialize).

    // https://fetch.spec.whatwg.org/#fetch-controller-next-manual-redirect-steps
    // next manual redirect steps (default null)
    //     Null or an algorithm accepting nothing.
    JS::GCPtr<JS::HeapFunction<void()>> m_next_manual_redirect_steps;

    JS::GCPtr<FetchParams> m_fetch_params;

    HashMap<u64, HTML::TaskID> m_ongoing_fetch_tasks;
    u64 m_next_fetch_task_id { 0 };
};

}
