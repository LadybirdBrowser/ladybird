/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebLocks/AbstractOperations.h>

namespace Web::WebLocks {

// https://w3c.github.io/web-locks/#lock-request
class LockRequest final : public JS::Cell {
    GC_CELL(LockRequest, JS::Cell);
    GC_DECLARE_ALLOCATOR(LockRequest);

public:
    LockRequest(String client_id, GC::Ref<LockManager>, String name, Bindings::LockMode, GC::Ref<WebIDL::CallbackType>, GC::Ref<WebIDL::Promise>, GC::Ptr<DOM::AbortSignal>);

    void abort_request();
    void signal_to_abort_request(GC::Ref<DOM::AbortSignal> signal);

    bool is_grantable(LockRequestQueue const&) const;

    String const& client_id() const { return m_client_id; }
    LockManager& manager() const { return m_manager; }

    String const& name() const { return m_name; }
    Bindings::LockMode mode() const { return m_mode; }

    GC::Ref<WebIDL::CallbackType> callback() const { return m_callback; }
    GC::Ref<WebIDL::Promise> promise() const { return m_promise; }

    GC::Ptr<DOM::AbortSignal> signal() const { return m_signal; }
    Optional<DOM::AbortSignal::AbortAlgorithmID> abort_algorithm_id() const { return m_abort_algorithm_id; }
    void set_abort_algorithm_id(Optional<DOM::AbortSignal::AbortAlgorithmID> abort_algorithm_id) { m_abort_algorithm_id = abort_algorithm_id; }

private:
    virtual void visit_edges(Cell::Visitor& visitor) override;

    String m_client_id;
    GC::Ref<LockManager> m_manager;
    String m_name;
    Bindings::LockMode m_mode;
    GC::Ref<WebIDL::CallbackType> m_callback;
    GC::Ref<WebIDL::Promise> m_promise;
    GC::Ptr<DOM::AbortSignal> m_signal;
    Optional<DOM::AbortSignal::AbortAlgorithmID> m_abort_algorithm_id;
};

}
