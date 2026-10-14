/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/WeakPtr.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

namespace Web::Fetch::Fetching {

class PendingResponse;

}

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-controller
class WEB_API FetchController : public JS::Cell {
    GC_CELL(FetchController, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchController);

public:
    enum class State : u8 {
        Ongoing,
        Terminated,
        Aborted,
        Stopped,
    };

    [[nodiscard]] static GC::Ref<FetchController> create();

    void set_full_timing_info(NonnullRefPtr<FetchTimingInfo> full_timing_info) { m_full_timing_info = move(full_timing_info); }
    void set_report_timing_steps(Function<void(JS::Object&)> report_timing_steps);
    void set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps);

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] RefPtr<FetchTimingInfo> timing_info() const;

    void report_timing(JS::Object&) const;
    void process_next_manual_redirect() const;

    // Runs the steps once the network has delivered all of the body whose bytes arrive through the channel, in place of
    // any steps set before.
    void when_network_completes_body(Engine::FetchByteChannel const&, GC::Ref<GC::Function<void()>> steps);
    void did_complete_network_body(Engine::FetchByteChannel const&);
    [[nodiscard]] NonnullRefPtr<FetchTimingInfo> extract_full_timing_info() const;
    void abort(JS::Realm&, Optional<JS::Value>);
    Optional<HTML::IPCSerializationRecord> const& serialized_abort_reason() const { return m_serialized_abort_reason; }
    void terminate();

    void set_fetch_params(Badge<FetchParams>, GC::Ref<FetchParams> fetch_params) { m_fetch_params = fetch_params; }
    [[nodiscard]] GC::Ptr<FetchParams> fetch_params() const { return m_fetch_params; }

    void set_algorithms(Badge<FetchParams>, GC::Ref<FetchAlgorithms const> algorithms) { m_algorithms = algorithms; }
    [[nodiscard]] GC::Ref<FetchAlgorithms const> algorithms() const;

    [[nodiscard]] TaskDestination const& task_destination() const { return m_task_destination; }
    void set_task_destination(TaskDestination task_destination) { m_task_destination = move(task_destination); }

    // AD-HOC: Whether this fetch's response has arrived: as headers from the network, or at main fetch. From then on,
    //         its body delivery is queued as event-loop tasks, and its response processing captures its task
    //         destination as it was then — so re-targeting the fetch no longer reaches either.
    //         consume_a_preloaded_resource() checks this before re-targeting an in-flight preload's fetch.
    void set_response_arrived() { m_response_arrived = true; }
    [[nodiscard]] bool response_arrived() const { return m_response_arrived; }
    [[nodiscard]] GC::Ptr<Fetching::PendingResponse> pending_preloaded_response() const { return m_pending_preloaded_response; }
    void set_pending_preloaded_response(GC::Ptr<Fetching::PendingResponse> pending_preloaded_response) { m_pending_preloaded_response = pending_preloaded_response; }

    void set_network_transport(Engine::NetworkTransportHandle&);
    void set_body_transport(Engine::FetchBodyDeliveryGate::Transport&);
    bool requires_network() const { return m_requires_network; }
    bool has_started_request() const { return m_has_started_request; }
    void set_inner_fetch_controller(GC::Ref<FetchController>);

    void stop_fetch();
    void stop_request();
    // Stops the delivery of the current response's body without ending the fetch.
    void stop_body_transport();
    void set_stop_steps(GC::Ref<GC::Function<void()>> stop_steps) { m_stop_steps = stop_steps; }

    u64 next_fetch_task_id() { return m_next_fetch_task_id++; }
    void fetch_task_queued(u64 fetch_task_id, HTML::TaskID event_id);
    void fetch_task_complete(u64 fetch_task_id);

    void add_pending_response(Badge<Fetching::PendingResponse>, GC::Ref<Fetching::PendingResponse>);
    void remove_pending_response(Badge<Fetching::PendingResponse>, GC::Ref<Fetching::PendingResponse>);

private:
    FetchController();

    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    // https://fetch.spec.whatwg.org/#fetch-controller-state
    // state (default "ongoing")
    //    "ongoing", "terminated", or "aborted"
    State m_state { State::Ongoing };

    // https://fetch.spec.whatwg.org/#fetch-controller-full-timing-info
    // full timing info (default null)
    //    Null or a fetch timing info.
    RefPtr<FetchTimingInfo> m_full_timing_info;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // report timing steps (default null)
    //    Null or an algorithm accepting a global object.
    GC::Ptr<GC::Function<void(JS::Object&)>> m_report_timing_steps;

    struct AwaitedNetworkCompletion {
        NonnullRefPtr<Engine::FetchByteChannel const> body;
        GC::Ref<GC::Function<void()>> steps;
    };
    Optional<AwaitedNetworkCompletion> m_awaited_network_completion;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // serialized abort reason (default null)
    //     Null or a Record (result of StructuredSerialize).
    Optional<HTML::IPCSerializationRecord> m_serialized_abort_reason;

    // https://fetch.spec.whatwg.org/#fetch-controller-next-manual-redirect-steps
    // next manual redirect steps (default null)
    //     Null or an algorithm accepting nothing.
    GC::Ptr<GC::Function<void()>> m_next_manual_redirect_steps;

    GC::Ptr<FetchParams> m_fetch_params;

    // AD-HOC: The fetch params' algorithms are kept on their controller, which every copy of the fetch params shares.
    // https://fetch.spec.whatwg.org/#fetch-params-process-request-body
    // process request body chunk length (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-request-end-of-body
    // process request end-of-body (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-early-hints-response
    // process early hints response (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response
    // process response (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response-end-of-body
    // process response end-of-body (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response-consume-body
    // process response consume body (default null)
    //     Null or an algorithm.
    GC::Ptr<FetchAlgorithms const> m_algorithms;

    // AD-HOC: The fetch params' task destination is kept on their controller, which every copy of the fetch params
    //         shares.
    // https://fetch.spec.whatwg.org/#fetch-params-task-destination
    // task destination (default null)
    //     Null, a global object, or a parallel queue.
    TaskDestination m_task_destination;

    bool m_response_arrived { false };
    // NB: Assumes one waiting consumer for a pending preloaded response.
    // Widen this if preload handoff ever supports multiple consumers.
    GC::Ptr<Fetching::PendingResponse> m_pending_preloaded_response;

    WeakPtr<Engine::FetchBodyDeliveryGate::Transport> m_body_transport;
    bool m_requires_network { false };
    bool m_has_started_request { false };

    HashMap<u64, HTML::TaskID> m_ongoing_fetch_tasks;
    u64 m_next_fetch_task_id { 0 };

    // AD-HOC: Run when the fetch is stopped, for whatever was waiting on its response.
    GC::Ptr<GC::Function<void()>> m_stop_steps;

    // The pending responses of this fetch whose callbacks have yet to run.
    Vector<GC::Ref<Fetching::PendingResponse>> m_pending_responses;
};

class FetchControllerHolder : public JS::Cell {
    GC_CELL(FetchControllerHolder, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchControllerHolder);

public:
    static GC::Ref<FetchControllerHolder> create();

    [[nodiscard]] GC::Ptr<FetchController> const& controller() const { return m_controller; }
    void set_controller(GC::Ref<FetchController> controller) { m_controller = controller; }

private:
    FetchControllerHolder();

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<FetchController> m_controller;
};

}
