/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>
#include <LibWeb/ServiceWorker/Job.h>
#include <LibWeb/ServiceWorker/Registration.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

static void run_job(JS::VM&, JobQueue&);
static void finish_job(JS::VM&, JS::NonnullGCPtr<Job>);
static void resolve_job_promise(JS::NonnullGCPtr<Job>, Optional<Registration const&>, JS::Value = JS::js_null());
template<typename Error>
static void reject_job_promise(JS::NonnullGCPtr<Job>, FlyString message);
static void register_(JS::VM&, JS::NonnullGCPtr<Job>);
static void update(JS::VM&, JS::NonnullGCPtr<Job>);
static void unregister(JS::VM&, JS::NonnullGCPtr<Job>);

JS_DEFINE_ALLOCATOR(Job);

// https://w3c.github.io/ServiceWorker/#create-job-algorithm
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

// https://w3c.github.io/ServiceWorker/#register-algorithm
static void register_(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    auto script_origin = job->script_url.origin();
    auto scope_origin = job->scope_url.origin();
    auto referrer_origin = job->referrer->origin();

    // 1. If the result of running potentially trustworthy origin with the origin of job’s script url as the argument is Not Trusted, then:
    if (SecureContexts::Trustworthiness::NotTrustworthy == SecureContexts::is_origin_potentially_trustworthy(script_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has untrustworthy script origin"_fly_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 2. If job’s script url's origin and job’s referrer's origin are not same origin, then:
    if (!script_origin.is_same_origin(referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has incompatible script and referrer origins"_fly_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 3. If job’s scope url's origin and job’s referrer's origin are not same origin, then:
    if (!scope_origin.is_same_origin(referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has incompatible scope and referrer origins"_fly_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 4. Let registration be the result of running Get Registration given job’s storage key and job’s scope url.
    auto registration = Registration::get(job->storage_key, job->scope_url);

    // 5. If registration is not null, then:
    if (registration.has_value()) {
        // 1. Let newestWorker be the result of running the Get Newest Worker algorithm passing registration as the argument.
        auto* newest_worker = registration->newest_worker();

        // 2. If newestWorker is not null, job’s script url equals newestWorker’s script url,
        //    job’s worker type equals newestWorker’s type, and job’s update via cache mode's value equals registration’s update via cache mode, then:
        if (newest_worker != nullptr
            && job->script_url == newest_worker->script_url
            && job->worker_type == newest_worker->worker_type
            && job->update_via_cache == registration->update_via_cache()) {
            // 1. Invoke Resolve Job Promise with job and registration.
            resolve_job_promise(job, registration.value());

            // 2. Invoke Finish Job with job and abort these steps.
            finish_job(vm, job);
            return;
        }
    }
    // 6. Else:
    else {
        // 1. Invoke Set Registration algorithm with job’s storage key, job’s scope url, and job’s update via cache mode.
        Registration::set(job->storage_key, job->scope_url, job->update_via_cache);
    }

    // Invoke Update algorithm passing job as the argument.
    update(vm, job);
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

// https://w3c.github.io/ServiceWorker/#run-job-algorithm
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

// https://w3c.github.io/ServiceWorker/#finish-job-algorithm
static void finish_job(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // 1. Let jobQueue be job’s containing job queue.
    auto& job_queue = *job->containing_job_queue;

    // 2. Assert: the first item in jobQueue is job.
    VERIFY(job_queue.first() == job);

    // 3. Dequeue from jobQueue
    (void)job_queue.take_first();

    // 4. If jobQueue is not empty, invoke Run Job with jobQueue.
    if (!job_queue.is_empty())
        run_job(vm, job_queue);
}

// https://w3c.github.io/ServiceWorker/#resolve-job-promise-algorithm
static void resolve_job_promise(JS::NonnullGCPtr<Job> job, Optional<Registration const&>, JS::Value value)
{
    // 1. If job’s client is not null, queue a task, on job’s client's responsible event loop using the DOM manipulation task source, to run the following substeps:
    if (job->client) {
        auto& realm = job->client->realm();
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, job->client->responsible_event_loop(), nullptr, JS::create_heap_function(realm.heap(), [&realm, job, value] {
            HTML::TemporaryExecutionContext const context(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            // FIXME: Resolve to a ServiceWorkerRegistration platform object
            // 1. Let convertedValue be null.
            // 2. If job’s job type is either register or update, set convertedValue to the result of
            //    getting the service worker registration object that represents value in job’s client.
            // 3. Else, set convertedValue to value, in job’s client's Realm.
            // 4. Resolve job’s job promise with convertedValue.
            WebIDL::resolve_promise(realm, *job->job_promise, value);
        }));
    }

    // 2. For each equivalentJob in job’s list of equivalent jobs:
    for (auto& equivalent_job : job->list_of_equivalent_jobs) {
        // 1. If equivalentJob’s client is null, continue.
        if (!equivalent_job->client)
            continue;

        // 2. Queue a task, on equivalentJob’s client's responsible event loop using the DOM manipulation task source,
        //    to run the following substeps:
        auto& realm = equivalent_job->client->realm();
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, equivalent_job->client->responsible_event_loop(), nullptr, JS::create_heap_function(realm.heap(), [&realm, equivalent_job, value] {
            HTML::TemporaryExecutionContext const context(*equivalent_job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            // FIXME: Resolve to a ServiceWorkerRegistration platform object
            // 1. Let convertedValue be null.
            // 2. If equivalentJob’s job type is either register or update, set convertedValue to the result of
            //    getting the service worker registration object that represents value in equivalentJob’s client.
            // 3. Else, set convertedValue to value, in equivalentJob’s client's Realm.
            // 4. Resolve equivalentJob’s job promise with convertedValue.
            WebIDL::resolve_promise(realm, *equivalent_job->job_promise, value);
        }));
    }
}

// https://w3c.github.io/ServiceWorker/#reject-job-promise-algorithm
template<typename Error>
static void reject_job_promise(JS::NonnullGCPtr<Job> job, FlyString message)
{
    // 1. If job’s client is not null, queue a task, on job’s client's responsible event loop using the DOM manipulation task source,
    //    to reject job’s job promise with a new exception with errorData and a user agent-defined message, in job’s client's Realm.
    if (job->client) {
        auto& realm = job->client->realm();
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, job->client->responsible_event_loop(), nullptr, JS::create_heap_function(realm.heap(), [&realm, job, message] {
            HTML::TemporaryExecutionContext const context(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::reject_promise(realm, *job->job_promise, Error::create(realm, message));
        }));
    }

    // 2. For each equivalentJob in job’s list of equivalent jobs:
    for (auto& equivalent_job : job->list_of_equivalent_jobs) {
        // 1. If equivalentJob’s client is null, continue.
        if (!equivalent_job->client)
            continue;

        // 2. Queue a task, on equivalentJob’s client's responsible event loop using the DOM manipulation task source,
        //    to reject equivalentJob’s job promise with a new exception with errorData and a user agent-defined message,
        //    in equivalentJob’s client's Realm.
        auto& realm = equivalent_job->client->realm();
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, equivalent_job->client->responsible_event_loop(), nullptr, JS::create_heap_function(realm.heap(), [&realm, equivalent_job, message] {
            HTML::TemporaryExecutionContext const context(*equivalent_job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::reject_promise(realm, *equivalent_job->job_promise, Error::create(realm, message));
        }));
    }
}

// https://w3c.github.io/ServiceWorker/#schedule-job-algorithm
void schedule_job(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // 1. Let jobQueue be null.
    // Note: See below for how we ensure job queue

    // 2. Let jobScope be job’s scope url, serialized.
    // FIXME: Suspect that spec should specify to not use fragment here
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
