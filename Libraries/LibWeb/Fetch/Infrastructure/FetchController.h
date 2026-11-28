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
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkActivity.h>
#include <LibWeb/Fetch/Engine/NetworkInterception.h>
#include <LibWeb/Fetch/Infrastructure/FetchControlBlock.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/NetworkObservation.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

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

    void set_report_timing_steps(GC::Ref<GC::Function<void(JS::Object&)>> report_timing_steps);
    void set_next_manual_redirect_steps(GC::Ref<GC::Function<void()>> next_manual_redirect_steps);

    [[nodiscard]] State state() const { return m_control_block->state(); }
    [[nodiscard]] RefPtr<FetchTimingInfo> full_timing_info() const { return m_control_block->full_timing_info(); }

    [[nodiscard]] bool has_report_timing_steps() const { return m_report_timing_steps != nullptr; }
    void report_timing(JS::Object&) const;
    void process_next_manual_redirect() const;

    // Posts the answer to the manual redirect handed over last, unless it was answered before.
    void resolve_redirect(Engine::Command::ResolveRedirect::Resolution);

    void resolve_network_interception(Engine::ContinuationId, Engine::NetworkDecision);
    [[nodiscard]] bool has_unanswered_redirect() const { return m_redirect_continuation_id.has_value(); }
    // The continuation of the manual redirect handed over last, if it waits for an answer.
    void set_redirect_continuation_id(Optional<Engine::ContinuationId> continuation_id) { m_redirect_continuation_id = continuation_id; }

    // Once the network has delivered all of the delivery's body, runs the steps given its end time, unless the fetch has
    // ended. An abort, termination, stop or later call drops it.
    void when_body_ends(Engine::DeliveryId, GC::Ptr<GC::Function<void(HighResolutionTime::DOMHighResTimeStamp)>> steps);
    void did_end_body(Engine::Event::EndOfBody const&);
    void did_end_fetch();
    [[nodiscard]] NonnullRefPtr<FetchTimingInfo> extract_full_timing_info() const;
    void abort(JS::Realm&, Optional<JS::Value>);
    Optional<HTML::IPCSerializationRecord> const& serialized_abort_reason() const { return m_serialized_abort_reason; }
    void terminate();

    // Creates the controller's fetch params, whose fetch runs the algorithms. Called once.
    [[nodiscard]] NonnullRefPtr<FetchParams> create_fetch_params(NonnullRefPtr<Request>, GC::Ref<FetchAlgorithms const>, NonnullRefPtr<FetchTimingInfo>, NonnullRefPtr<LoaderConfigSnapshot const>);

    [[nodiscard]] GC::Ref<FetchAlgorithms const> algorithms() const;

    // The request the controller's fetch params were created with.
    [[nodiscard]] NonnullRefPtr<Request> request() const;

    // The realm that started the fetch.
    void set_realm(JS::Realm& realm) { m_realm = realm; }
    [[nodiscard]] JS::Realm& realm() const
    {
        VERIFY(m_realm);
        return *m_realm;
    }

    // AD-HOC: Whether the preload whose response the fetch consumes reports its own timing to Resource Timing.
    [[nodiscard]] bool preload_reports_timing() const { return m_preload_reports_timing; }
    void set_preload_reports_timing(bool preload_reports_timing) { m_preload_reports_timing = preload_reports_timing; }

    // The network observation of the request the controller's fetch params were created with.
    [[nodiscard]] RefPtr<NetworkObservation> const& network_observation() const { return m_network_observation; }

    // The network observation of one of the fetch's CORS preflights, which the controller keeps from the preflight's
    // first network event on.
    [[nodiscard]] NonnullRefPtr<NetworkObservation> cors_preflight_network_observation(Engine::CorsPreflightId);

    // The fetch's own request as its latest network event reported it, or as it was when main fetch handed the fetch to
    // its engine.
    [[nodiscard]] Optional<Engine::Event::ObservedRequest> const& observed_request() const { return m_observed_request; }
    void set_observed_request(Engine::Event::ObservedRequest observed_request) { m_observed_request = move(observed_request); }

    [[nodiscard]] TaskDestination const& task_destination() const { return m_task_destination; }
    void set_task_destination(TaskDestination task_destination) { m_task_destination = move(task_destination); }

    // The page of the realm that started the fetch.
    [[nodiscard]] GC::Ref<Page> page() const;
    void set_page(Page& page) { m_page = page; }

    // Moves the fetch, before its response has arrived, to another task destination and body intent.
    void retarget(TaskDestination, Engine::BodyIntent);

    [[nodiscard]] bool is_keepalive() const { return m_is_keepalive; }
    void set_keepalive(bool is_keepalive) { m_is_keepalive = is_keepalive; }

    [[nodiscard]] bool response_arrived() const { return m_control_block->response_arrived(); }

    bool requires_network() const { return m_control_block->requires_network(); }
    bool has_started_request() const { return m_control_block->has_started_request(); }
    // The fetch this controller's fetch params started, and the executor its start was posted to. The controller's
    // stops and redirect answers are posted there, for that fetch.
    void set_started_fetch(Engine::FetchId, Engine::FetchExecutor&);
    [[nodiscard]] Optional<Engine::FetchId> started_fetch_id() const;
    [[nodiscard]] Engine::FetchExecutor& started_fetch_executor() const;

    void stop_fetch();
    void stop_request();
    void set_stop_steps(GC::Ref<GC::Function<void()>> stop_steps) { m_stop_steps = stop_steps; }

    void queue_fetch_task(GC::Ref<GC::Function<void()>>);

private:
    explicit FetchController(FetchControllerRegistry&);

    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    WeakPtr<FetchControllerRegistry> m_registry;
    FetchControllerToken m_token;
    NonnullRefPtr<FetchControlBlock> m_control_block;
    RefPtr<Request> m_request;
    GC::Ptr<JS::Realm> m_realm;
    RefPtr<NetworkObservation> m_network_observation;
    HashMap<Engine::CorsPreflightId, NonnullRefPtr<NetworkObservation>> m_cors_preflight_network_observations;
    Optional<Engine::Event::ObservedRequest> m_observed_request;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // report timing steps (default null)
    //    Null or an algorithm accepting a global object.
    GC::Ptr<GC::Function<void(JS::Object&)>> m_report_timing_steps;

    struct AwaitedEndOfBody {
        Engine::DeliveryId delivery_id;
        GC::Ptr<GC::Function<void(HighResolutionTime::DOMHighResTimeStamp)>> steps;
    };
    Optional<AwaitedEndOfBody> m_awaited_end_of_body;
    bool m_fetch_ended { false };

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // serialized abort reason (default null)
    //     Null or a Record (result of StructuredSerialize).
    Optional<HTML::IPCSerializationRecord> m_serialized_abort_reason;

    // https://fetch.spec.whatwg.org/#fetch-controller-next-manual-redirect-steps
    // next manual redirect steps (default null)
    //     Null or an algorithm accepting nothing.
    GC::Ptr<GC::Function<void()>> m_next_manual_redirect_steps;

    // The continuation of the manual redirect opened last, until it is answered.
    Optional<Engine::ContinuationId> m_redirect_continuation_id;

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
    GC::Ptr<Page> m_page;

    bool m_is_keepalive { false };
    bool m_preload_reports_timing { false };

    struct StartedFetch {
        Engine::FetchId fetch_id;
        NonnullRefPtr<Engine::FetchExecutor> fetch_executor;
    };
    Optional<StartedFetch> m_started_fetch;

    HashMap<u64, HTML::TaskID> m_ongoing_fetch_tasks;
    u64 m_next_fetch_task_id { 0 };

    // AD-HOC: Run when the fetch is stopped, for whatever was waiting on its response.
    GC::Ptr<GC::Function<void()>> m_stop_steps;
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
