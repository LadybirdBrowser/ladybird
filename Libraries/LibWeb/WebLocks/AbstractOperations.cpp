/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebLocks/AbstractOperations.h>

namespace Web::WebLocks {

extern "C" {
void web_locks_queue_task(JS::Realm*, GC::Cell*, void (*steps)(JSVM*, void*));
void web_locks_queue_callback_task(WebIDL::CallbackType*, GC::Cell*, void (*steps)(JSVM*, void*));
void web_locks_queue_notices_task(void (*steps)(JSVM*));

u64 web_rust_web_locks_agent_id();
void web_rust_web_locks_terminate_remaining_locks_and_requests_of_agent(u64 agent_id);
void web_rust_web_locks_terminate_remaining_locks_and_requests_of_client(JSVM*, HTML::EnvironmentSettingsObject*);
}

// https://w3c.github.io/web-locks/#lock-task-queue
static HTML::ParallelQueue& lock_task_queue()
{
    // A user agent has a lock task queue which is the result of starting a new parallel queue.
    // NB: A parallel queue queues tasks on the agent that uses it, so this is one queue per agent. The lock managers that
    //     agents share are in a table behind a lock, and each agent acts on its own requests and locks from its own queue.
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

// NB: The steps act on objects of more than one realm, and enter the realm of each themselves.
void web_locks_queue_notices_task(void (*steps)(JSVM*))
{
    auto& vm = JS::VM::the();
    lock_task_queue().enqueue(GC::create_function(vm.heap(), GC::captures(steps), [](auto& steps) {
        steps(JS::HostABI::vm_to_abi(JS::VM::the()));
    }));
}

u64 lock_agent_id()
{
    return web_rust_web_locks_agent_id();
}

void terminate_remaining_locks_and_requests(u64 lock_agent_id)
{
    web_rust_web_locks_terminate_remaining_locks_and_requests_of_agent(lock_agent_id);
}

void run_unloading_cleanup_steps(DOM::Document& document)
{
    // Whenever the unloading document cleanup steps run with a document, terminate remaining locks and requests with its
    // agent.
    // NB: The documents of this process share one agent, so this terminates the locks and requests of the document's
    //     environment only. A window that another document takes over keeps its environment, and so its locks.
    auto window = document.window();
    if (!window || &window->associated_document() != &document || document.window_is_taken_over_after_unload())
        return;
    auto& environment = HTML::relevant_settings_object(*window);
    web_rust_web_locks_terminate_remaining_locks_and_requests_of_client(JS::HostABI::vm_to_abi(JS::VM::the()), &environment);
}

}
