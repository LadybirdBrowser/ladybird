/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/LockManager.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/WebLocks/Lock.h>
#include <LibWeb/WebLocks/LockManager.h>
#include <LibWeb/WebLocks/LockRequest.h>

namespace Web::WebLocks {

GC_DEFINE_ALLOCATOR(LockRequest);

LockRequest::LockRequest(String client_id, GC::Ref<LockManager> manager, String name, Bindings::LockMode mode, GC::Ref<WebIDL::CallbackType> callback, GC::Ref<WebIDL::Promise> promise, GC::Ptr<DOM::AbortSignal> signal)
    : m_client_id(move(client_id))
    , m_manager(manager)
    , m_name(move(name))
    , m_mode(mode)
    , m_callback(callback)
    , m_promise(promise)
    , m_signal(signal)
{
}

void LockRequest::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_manager);
    visitor.visit(m_callback);
    visitor.visit(m_promise);
    visitor.visit(m_signal);
}

// https://w3c.github.io/web-locks/#abort-the-request
void LockRequest::abort_request()
{
    // 1. Assert: these steps are running on the lock task queue.

    // 2. Let manager be request’s manager.
    // 3. Let name be request’s name.
    // 4. Let queueMap be manager’s lock request queue map.
    auto& queue_map = m_manager->lock_request_queue_map();

    // 5. Let queue be the result of getting the lock request queue from queueMap for name.
    auto& queue = get_lock_request_queue(queue_map, m_name);

    // 6. Remove request from queue.
    queue.remove_first_matching([&](GC::Ref<LockRequest> queued_request) {
        return queued_request == this;
    });

    // 7. Process the lock request queue queue.
    m_manager->process_lock_request_queue(queue);
}

// https://w3c.github.io/web-locks/#signal-to-abort-the-request
void LockRequest::signal_to_abort_request(GC::Ref<DOM::AbortSignal> signal)
{
    // 1. Enqueue the steps to abort the request request to the lock task queue.
    queue_web_locks_task(signal->realm(), GC::create_function(heap(), [this]() {
        abort_request();
    }));

    // 2. Reject request’s promise with signal’s abort reason.
    WebIDL::reject_promise(signal->realm(), m_promise, signal->reason());
}

// https://w3c.github.io/web-locks/#grantable
bool LockRequest::is_grantable(LockRequestQueue const& queue) const
{
    // 1. Let manager be request’s manager.
    // 2. Let queueMap be manager’s lock request queue map.
    // 3. Let name be request’s name.
    // 4. Let queue be the result of getting the lock request queue from queueMap for name.
    // NB: Every caller already has the queue this request belongs to. We avoid repeating an invocation to get the queue
    //     from the queue map here as the repeated HashMap::ensure might rehash the table and invalidate references.

    // 5. Let held be manager’s held lock set
    auto const& held = m_manager->held_lock_set();

    // 6. Let mode be request’s mode

    // 7. If queue is not empty and request is not the first item in queue, then return false.
    if (!queue.is_empty() && queue.first() != this)
        return false;

    // 8. If mode is "exclusive", then return true if no lock in held has name equal to name, and false otherwise.
    if (m_mode == Bindings::LockMode::Exclusive) {
        return !held.contains([&](GC::Ref<LockData> lock) {
            return lock->name() == m_name;
        });
    }

    // 9. Otherwise, mode is "shared"; return true if no lock in held has mode "exclusive" and has name equal to name,
    //    and false otherwise.
    return !held.contains([&](GC::Ref<LockData> lock) {
        return lock->mode() == Bindings::LockMode::Exclusive && lock->name() == m_name;
    });
}

}
