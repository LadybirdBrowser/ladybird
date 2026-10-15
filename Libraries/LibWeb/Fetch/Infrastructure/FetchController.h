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
#include <LibWeb/Fetch/Infrastructure/FetchControlBlock.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
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
    using State = FetchControlBlock::State;

    [[nodiscard]] static GC::Ref<FetchController> create(FetchControllerRegistry&);

    [[nodiscard]] FetchControllerToken token() const { return m_token; }
    [[nodiscard]] WeakPtr<FetchControllerRegistry> const& registry() const { return m_registry; }
    [[nodiscard]] FetchControlBlock& control_block() const { return *m_control_block; }

    void set_report_timing_steps(Function<void(JS::Object&)> report_timing_steps);
    void set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps);

    [[nodiscard]] State state() const { return m_control_block->state(); }
    [[nodiscard]] RefPtr<FetchTimingInfo> full_timing_info() const { return m_control_block->full_timing_info(); }

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

    void set_algorithms(Badge<FetchParams>, GC::Ref<FetchAlgorithms const>);
    [[nodiscard]] GC::Ref<FetchAlgorithms const> algorithms() const;

    // The fetch params created with the controller, for which WebDriver BiDi reports errors and intercepts requests.
    void set_original_fetch_params(Badge<FetchParams>, FetchParams const&);
    [[nodiscard]] RefPtr<FetchParams> original_fetch_params() const;

    [[nodiscard]] TaskDestination const& task_destination() const { return m_task_destination; }
    void set_task_destination(TaskDestination task_destination) { m_task_destination = move(task_destination); }

    // Moves the fetch, before its response has arrived, to another task destination and body intent.
    void retarget(TaskDestination, Engine::BodyIntent);

    [[nodiscard]] bool is_keepalive() const { return m_is_keepalive; }
    void set_keepalive(bool is_keepalive) { m_is_keepalive = is_keepalive; }

    [[nodiscard]] bool response_arrived() const { return m_control_block->response_arrived(); }
    [[nodiscard]] GC::Ptr<Fetching::PendingResponse> pending_preloaded_response() const { return m_pending_preloaded_response; }
    void set_pending_preloaded_response(GC::Ptr<Fetching::PendingResponse> pending_preloaded_response) { m_pending_preloaded_response = pending_preloaded_response; }

    void set_network_transport(Engine::NetworkTransportHandle&);
    void set_body_transport(Engine::FetchBodyDeliveryGate::Transport&);
    bool requires_network() const { return m_control_block->requires_network(); }
    bool has_started_request() const { return m_control_block->has_started_request(); }
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
    explicit FetchController(FetchControllerRegistry&);

    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    void report_webdriver_bidi_fetch_error(StringView error_text) const;
    void cancel_webdriver_bidi_network_interception() const;

    WeakPtr<FetchControllerRegistry> m_registry;
    FetchControllerToken m_token;
    NonnullRefPtr<FetchControlBlock> m_control_block;
    WeakPtr<FetchParams> m_original_fetch_params;

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
    bool m_is_keepalive { false };

    // NB: Assumes one waiting consumer for a pending preloaded response.
    // Widen this if preload handoff ever supports multiple consumers.
    GC::Ptr<Fetching::PendingResponse> m_pending_preloaded_response;

    WeakPtr<Engine::FetchBodyDeliveryGate::Transport> m_body_transport;

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
