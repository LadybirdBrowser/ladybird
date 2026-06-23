/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebLocks/AbstractOperations.h>

namespace Web::WebLocks {

class LockManager final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(LockManager, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(LockManager);

public:
    static GC::Ref<LockManager> create(JS::Realm&);
    virtual ~LockManager() override = default;

    GC::Ref<WebIDL::Promise> request(String const& name, GC::Ref<WebIDL::CallbackType>);
    GC::Ref<WebIDL::Promise> request(String const& name, Bindings::LockOptions const&, GC::Ref<WebIDL::CallbackType>);
    GC::Ref<WebIDL::Promise> query();

    void process_lock_request_queue(LockRequestQueue&);

    LockRequestQueueMap& lock_request_queue_map() { return m_lock_request_queue_map; }
    Vector<GC::Ref<LockData>>& held_lock_set() { return m_held_lock_set; }

private:
    explicit LockManager(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<LockRequest> request_lock(GC::Ref<WebIDL::Promise>, GC::Ref<WebIDL::CallbackType>, String name, Bindings::LockMode, bool if_available, bool steal, GC::Ptr<DOM::AbortSignal>);

    void snapshot_lock_state(GC::Ref<WebIDL::Promise>);

    // https://w3c.github.io/web-locks/#lock-manager-held-lock-set
    Vector<GC::Ref<LockData>> m_held_lock_set;

    // https://w3c.github.io/web-locks/#lock-manager-lock-request-queue-map
    LockRequestQueueMap m_lock_request_queue_map;
};

}
