/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebLocks/AbstractOperations.h>

namespace Web::WebLocks {

// https://w3c.github.io/web-locks/#lock-task-queue
HTML::ParallelQueue& lock_task_queue()
{
    // A user agent has a lock task queue which is the result of starting a new parallel queue.
    static NeverDestroyed<NonnullRefPtr<HTML::ParallelQueue>> queue { HTML::ParallelQueue::create() };
    return **queue;
}

void queue_web_locks_task(JS::Realm& realm, GC::Ref<GC::Function<void()>> steps)
{
    lock_task_queue().enqueue(GC::create_function(realm.heap(), [realm = GC::Ref { realm }, steps]() {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps->function()();
    }));
}

void queue_web_locks_task_on_relevant_event_loop(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback, GC::Ref<GC::Function<void()>> steps)
{
    HTML::queue_global_task(HTML::Task::Source::WebLocks, callback->callback_context->global_object(), GC::create_function(realm.heap(), [callback, steps]() {
        HTML::TemporaryExecutionContext execution_context { callback->callback_context, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps->function()();
    }));
}

// https://w3c.github.io/web-locks/#obtain-a-lock-manager
LockManager& obtain_lock_manager(HTML::EnvironmentSettingsObject& environment)
{
    // 1. Let map be the result of obtaining a local storage bottle map given environment and "web-locks".
    // 2. If map is failure, then return failure.
    // 3. Let bottle be map’s associated storage bottle.
    // 4. Return bottle’s associated lock manager.

    // FIXME: Spec issue: Storing and obtaining a lock manager in the environment's storage is currently not well
    //        defined. So we store a single instance per environment for now. See:
    //        https://w3c.github.io/web-locks/#issue-73644ca1
    return environment.lock_manager();
}

// https://w3c.github.io/web-locks/#get-the-lock-request-queue
LockRequestQueue& get_lock_request_queue(LockRequestQueueMap& queue_map, String const& name)
{
    // 1. If queueMap[name] does not exist, set queueMap[name] to a new empty lock request queue.
    // 2. Return queueMap[name].
    return queue_map.ensure(name);
}

}
