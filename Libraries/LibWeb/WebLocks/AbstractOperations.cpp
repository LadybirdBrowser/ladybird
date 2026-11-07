/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::WebLocks {

extern "C" {
void web_locks_queue_task(JS::Realm*, GC::Cell*, void (*steps)(JSVM*, void*));
void web_locks_queue_callback_task(WebIDL::CallbackType*, GC::Cell*, void (*steps)(JSVM*, void*));
}

// https://w3c.github.io/web-locks/#lock-task-queue
static HTML::ParallelQueue& lock_task_queue()
{
    // A user agent has a lock task queue which is the result of starting a new parallel queue.
    // NB: Lock managers are per environment, and a parallel queue queues tasks on the agent that uses it. So this is one
    //     queue per agent.
    auto& agent = HTML::surrounding_agent();
    if (!agent.lock_task_queue)
        agent.lock_task_queue = HTML::ParallelQueue::create();
    return *agent.lock_task_queue;
}

void web_locks_queue_task(JS::Realm* realm, GC::Cell* cell, void (*steps)(JSVM*, void*))
{
    lock_task_queue().enqueue(GC::create_function(realm->heap(), GC::captures(GC::Ref { *realm }, GC::Ref { *cell }, steps), [](auto& realm, auto& cell, auto& steps) {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps(JS::HostABI::vm_to_abi(realm->vm()), cell.ptr());
    }));
}

void web_locks_queue_callback_task(WebIDL::CallbackType* callback, GC::Cell* cell, void (*steps)(JSVM*, void*))
{
    auto& callback_context = callback->callback_context;
    HTML::queue_global_task(HTML::Task::Source::WebLocks, callback_context->global_object(), GC::create_function(callback_context->realm().heap(), GC::captures(GC::Ref { *callback }, GC::Ref { *cell }, steps), [](auto& callback, auto& cell, auto& steps) {
        HTML::TemporaryExecutionContext execution_context { callback->callback_context, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps(JS::HostABI::vm_to_abi(callback->callback_context->realm().vm()), cell.ptr());
    }));
}

}
