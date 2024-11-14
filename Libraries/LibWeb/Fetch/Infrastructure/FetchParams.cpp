/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchParams);

FetchParams::FetchParams(GC::Ref<Request> request, GC::Ref<FetchAlgorithms> algorithms, GC::Ref<FetchController> controller, GC::Ref<FetchTimingInfo> timing_info)
    : m_request(request)
    , m_algorithms(algorithms)
    , m_controller(controller)
    , m_timing_info(timing_info)
{
    m_controller->set_fetch_params({}, *this);
}

GC::Ref<FetchParams> FetchParams::create(JS::VM& vm, GC::Ref<Request> request, GC::Ref<FetchTimingInfo> timing_info)
{
    auto algorithms = Infrastructure::FetchAlgorithms::create(vm, {});
    auto controller = Infrastructure::FetchController::create(vm);
    return vm.heap().allocate<FetchParams>(request, algorithms, controller, timing_info);
}

void FetchParams::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request);
    visitor.visit(m_algorithms);
    visitor.visit(m_controller);
    visitor.visit(m_timing_info);
    if (m_task_destination.has<GC::Ref<JS::Object>>())
        visitor.visit(m_task_destination.get<GC::Ref<JS::Object>>());
    if (m_preloaded_response_candidate.has<GC::Ref<Response>>())
        visitor.visit(m_preloaded_response_candidate.get<GC::Ref<Response>>());
}

// https://fetch.spec.whatwg.org/#fetch-params-aborted
bool FetchParams::is_aborted() const
{
    // A fetch params fetchParams is aborted if its controller’s state is "aborted".
    return m_controller->state() == FetchController::State::Aborted;
}

// https://fetch.spec.whatwg.org/#fetch-params-canceled
bool FetchParams::is_canceled() const
{
    // A fetch params fetchParams is canceled if its controller’s state is "aborted" or "terminated".
    return m_controller->state() == FetchController::State::Aborted || m_controller->state() == FetchController::State::Terminated;
}

}
