/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/LockManager.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebLocks/AbstractOperations.h>

namespace Web::WebLocks {

class LockManager final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(LockManager, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(LockManager);

public:
    static GC::Ref<LockManager> create(HTML::EnvironmentSettingsObject&);
    virtual ~LockManager() override = default;

    GC::Ref<WebIDL::Promise> request(Utf16String const& name, GC::Ref<WebIDL::CallbackType>);
    GC::Ref<WebIDL::Promise> request(Utf16String const& name, Bindings::LockOptions const&, GC::Ref<WebIDL::CallbackType>);
    GC::Ref<WebIDL::Promise> query();

    void process_lock_request_queue(LockRequestQueue&);

    LockRequestQueueMap& lock_request_queue_map() { return m_lock_request_queue_map; }
    Vector<GC::Ref<LockData>>& held_lock_set() { return m_held_lock_set; }
    JS::Realm& relevant_realm() const { return m_environment->realm(); }

private:
    explicit LockManager(GC::Ref<HTML::EnvironmentSettingsObject>);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    GC::Ref<LockRequest> request_lock(GC::Ref<WebIDL::Promise>, GC::Ref<WebIDL::CallbackType>, Utf16String name, Bindings::LockMode, bool if_available, bool steal, GC::Ptr<DOM::AbortSignal>);

    void snapshot_lock_state(GC::Ref<WebIDL::Promise>);

    // https://w3c.github.io/web-locks/#lock-manager-held-lock-set
    Vector<GC::Ref<LockData>> m_held_lock_set;

    // https://w3c.github.io/web-locks/#lock-manager-lock-request-queue-map
    LockRequestQueueMap m_lock_request_queue_map;

    GC::Ref<HTML::EnvironmentSettingsObject> m_environment;
};

}
