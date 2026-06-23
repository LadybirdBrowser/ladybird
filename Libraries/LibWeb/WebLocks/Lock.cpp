/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Lock.h>
#include <LibWeb/Bindings/LockManager.h>
#include <LibWeb/WebLocks/Lock.h>
#include <LibWeb/WebLocks/LockManager.h>

namespace Web::WebLocks {

GC_DEFINE_ALLOCATOR(Lock);
GC_DEFINE_ALLOCATOR(LockData);

GC::Ref<Lock> Lock::create(JS::Realm& realm, GC::Ref<LockData> lock)
{
    return realm.create<Lock>(realm, lock);
}

Lock::Lock(JS::Realm& realm, GC::Ref<LockData> lock)
    : PlatformObject(realm)
    , m_lock(lock)
{
}

void Lock::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Lock);
    Base::initialize(realm);
}

void Lock::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_lock);
}

// https://w3c.github.io/web-locks/#dom-lock-name
String const& Lock::name() const
{
    // The name getter’s steps are to return the associated lock’s name.
    return m_lock->name();
}

// https://w3c.github.io/web-locks/#dom-lock-mode
Bindings::LockMode Lock::mode() const
{
    // The mode getter’s steps are to return the associated lock’s mode.
    return m_lock->mode();
}

LockData::LockData(String client_id, GC::Ref<LockManager> manager, Bindings::LockMode mode, String name, GC::Ref<WebIDL::Promise> released_promise, GC::Ref<WebIDL::Promise> waiting_promise)
    : m_client_id(move(client_id))
    , m_manager(manager)
    , m_name(move(name))
    , m_mode(mode)
    , m_released_promise(released_promise)
    , m_waiting_promise(waiting_promise)
{
}

void LockData::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_manager);
    visitor.visit(m_released_promise);
    visitor.visit(m_waiting_promise);
}

// https://w3c.github.io/web-locks/#release-the-lock
void LockData::release_lock() const
{
    // 1. Assert: these steps are running on the lock task queue.

    // 2. Let manager be lock’s manager.
    // 3. Let queueMap be manager’s lock request queue map.
    auto& queue_map = m_manager->lock_request_queue_map();

    // 4. Let name be lock’s resource name.
    // 5. Let queue be the result of getting the lock request queue from queueMap for name.
    auto& queue = get_lock_request_queue(queue_map, m_name);

    // 6. Remove lock from the manager’s held lock set.
    m_manager->held_lock_set().remove_first_matching([&](GC::Ref<LockData> held_lock) {
        return held_lock == this;
    });

    // 7. Process the lock request queue queue.
    m_manager->process_lock_request_queue(queue);
}

}
