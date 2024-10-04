/*
 * Copyright (c) 2024, Andrew Kaster <akaster@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/ServiceWorker/Job.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

JS_DEFINE_ALLOCATOR(Job);

// https://w3c.github.io/ServiceWorker/#create-job
JS::NonnullGCPtr<Job> Job::create(JS::VM& vm, Job::Type type, StorageAPI::StorageKey storage_key, URL::URL scope_url, URL::URL script_url, JS::GCPtr<WebIDL::Promise> promise, JS::GCPtr<HTML::EnvironmentSettingsObject> client)
{
    return vm.heap().allocate_without_realm<Job>(type, move(storage_key), move(scope_url), move(script_url), promise, client);
}

Job::Job(Job::Type type, StorageAPI::StorageKey storage_key, URL::URL scope_url, URL::URL script_url, JS::GCPtr<WebIDL::Promise> promise, JS::GCPtr<HTML::EnvironmentSettingsObject> client)
    : job_type(type)
    , storage_key(move(storage_key))
    , scope_url(move(scope_url))
    , script_url(move(script_url))
    , client(client)
    , job_promise(promise)
{
    // 8. If client is not null, set job’s referrer to client’s creation URL.
    if (client)
        referrer = client->creation_url;
}

Job::~Job() = default;

void Job::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(client);
    visitor.visit(job_promise);
    for (auto& job : list_of_equivalent_jobs)
        visitor.visit(job);
}

// FIXME: Does this need to be a 'user agent' level thing? Or can we have one per renderer process?
// https://w3c.github.io/ServiceWorker/#dfn-scope-to-job-queue-map
static HashMap<ByteString, JobQueue>& scope_to_job_queue_map()
{
    static HashMap<ByteString, JobQueue> map;
    return map;
}

static void register_(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // If there's no client, there won't be any promises to resolve
    if (job->client) {
        auto context = HTML::TemporaryExecutionContext(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto& realm = *vm.current_realm();
        WebIDL::reject_promise(realm, *job->job_promise, *vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Service Worker registration"sv).value());
    }
}

static void update(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // If there's no client, there won't be any promises to resolve
    if (job->client) {
        auto context = HTML::TemporaryExecutionContext(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto& realm = *vm.current_realm();
        WebIDL::reject_promise(realm, *job->job_promise, *vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Service Worker update"sv).value());
    }
}

static void unregister(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // If there's no client, there won't be any promises to resolve
    if (job->client) {
        auto context = HTML::TemporaryExecutionContext(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto& realm = *vm.current_realm();
        WebIDL::reject_promise(realm, *job->job_promise, *vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Service Worker unregistration"sv).value());
    }
}

// https://w3c.github.io/ServiceWorker/#run-job
static void run_job(JS::VM& vm, JobQueue& job_queue)
{
    // 1. Assert: jobQueue is not empty.
    VERIFY(!job_queue.is_empty());

    // 2. Queue a task to run these steps:
    auto job_run_steps = JS::create_heap_function(vm.heap(), [&vm, &job_queue] {
        // 1. Let job be the first item in jobQueue.
        auto& job = job_queue.first();

        // FIXME: Do these really need to be in parallel to the HTML event loop? Sounds fishy
        switch (job->job_type) {
        case Job::Type::Register:
            // 2. If job’s job type is register, run Register with job in parallel.
            register_(vm, job);
            break;
        case Job::Type::Update:
            // 3. If job’s job type is update, run Update with job in parallel.
            update(vm, job);
            break;
        case Job::Type::Unregister:
            // 4. If job’s job type is unregister, run Unregister with job in parallel.
            unregister(vm, job);
            break;
        }
    });

    // FIXME: How does the user agent ensure this happens? Is this a normative note?
    // Spec-Note:
    // For a register job and an update job, the user agent delays queuing a task for running the job
    // until after a DOMContentLoaded event has been dispatched to the document that initiated the job.

    // FIXME: Spec should be updated to avoid 'queue a task' and use 'queue a global task' instead
    // FIXME: On which task source? On which event loop? On behalf of which document?
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, job_run_steps);
}

// https://w3c.github.io/ServiceWorker/#schedule-job
void schedule_job(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // 1. Let jobQueue be null.
    // Note: See below for how we ensure job queue

    // 2. Let jobScope be job’s scope url, serialized.
    auto job_scope = job->scope_url.serialize();

    // 3. If scope to job queue map[jobScope] does not exist, set scope to job queue map[jobScope] to a new job queue.
    // 4. Set jobQueue to scope to job queue map[jobScope].
    auto& job_queue = scope_to_job_queue_map().ensure(job_scope, [&vm] {
        return JobQueue(vm.heap());
    });

    // 5. If jobQueue is empty, then:
    if (job_queue.is_empty()) {
        // 2. Set job’s containing job queue to jobQueue, and enqueue job to jobQueue.
        job->containing_job_queue = &job_queue;
        job_queue.append(job);
        run_job(vm, job_queue);
    }
    // 6. Else:
    else {
        // 1. Let lastJob be the element at the back of jobQueue.
        auto& last_job = job_queue.last();

        // 2. If job is equivalent to lastJob and lastJob’s job promise has not settled, append job to lastJob’s list of equivalent jobs.
        // FIXME: There's no WebIDL AO that corresponds to checking if an ECMAScript promise has settled
        if (job == last_job && !verify_cast<JS::Promise>(*job->job_promise->promise()).is_handled()) {
            last_job->list_of_equivalent_jobs.append(job);
        }
        // 3. Else, set job’s containing job queue to jobQueue, and enqueue job to jobQueue.
        else {
            job->containing_job_queue = &job_queue;
            job_queue.append(job);
        }
    }
}

}
