/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebLocks {

// https://w3c.github.io/web-locks/#lock
class Lock final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Lock, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Lock);

public:
    static GC::Ref<Lock> create(JS::Realm&, GC::Ref<LockData>);
    virtual ~Lock() override = default;

    String const& name() const;
    Bindings::LockMode mode() const;

private:
    Lock(JS::Realm&, GC::Ref<LockData>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor& visitor) override;

    // https://w3c.github.io/web-locks/#ref-for-lock③
    GC::Ref<LockData> m_lock;
};

// https://w3c.github.io/web-locks/#lock-concept
class LockData final : public JS::Cell {
    GC_CELL(LockData, JS::Cell);
    GC_DECLARE_ALLOCATOR(LockData);

public:
    LockData(String client_id, GC::Ref<LockManager>, Bindings::LockMode, String name, GC::Ref<WebIDL::Promise> released_promise, GC::Ref<WebIDL::Promise> waiting_promise);

    void release_lock() const;

    String const& client_id() const { return m_client_id; }
    String const& name() const { return m_name; }
    Bindings::LockMode mode() const { return m_mode; }

    GC::Ref<WebIDL::Promise> released_promise() const { return m_released_promise; }
    GC::Ref<WebIDL::Promise> waiting_promise() const { return m_waiting_promise; }

private:
    virtual void visit_edges(Cell::Visitor& visitor) override;

    // https://w3c.github.io/web-locks/#lock-concept-clientid
    String m_client_id;

    // https://w3c.github.io/web-locks/#lock-concept-manager
    GC::Ref<LockManager> m_manager;

    // https://w3c.github.io/web-locks/#lock-concept-name
    String m_name;

    // https://w3c.github.io/web-locks/#lock-concept-mode
    Bindings::LockMode m_mode;

    // https://w3c.github.io/web-locks/#lock-concept-released-promise
    GC::Ref<WebIDL::Promise> m_released_promise;

    // https://w3c.github.io/web-locks/#lock-concept-waiting-promise
    GC::Ref<WebIDL::Promise> m_waiting_promise;
};

}
