/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/URL.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/Script.h>
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
static void reject_job_promise(JS::NonnullGCPtr<Job>, String message);
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
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has untrustworthy script origin"_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 2. If job’s script url's origin and job’s referrer's origin are not same origin, then:
    if (!script_origin.is_same_origin(referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has incompatible script and referrer origins"_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 3. If job’s scope url's origin and job’s referrer's origin are not same origin, then:
    if (!scope_origin.is_same_origin(referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has incompatible scope and referrer origins"_string);

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

// Used to share internal Update algorithm state b/w fetch callbacks
class UpdateAlgorithmState : JS::Cell {
    JS_CELL(UpdateAlgorithmState, JS::Cell);

public:
    static JS::NonnullGCPtr<UpdateAlgorithmState> create(JS::VM& vm)
    {
        return vm.heap().allocate_without_realm<UpdateAlgorithmState>();
    }

    OrderedHashMap<URL::URL, JS::NonnullGCPtr<Fetch::Infrastructure::Response>>& updated_resource_map() { return m_map; }
    bool has_updated_resources() const { return m_has_updated_resources; }
    void set_has_updated_resources(bool b) { m_has_updated_resources = b; }

private:
    UpdateAlgorithmState() = default;

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_map);
    }

    OrderedHashMap<URL::URL, JS::NonnullGCPtr<Fetch::Infrastructure::Response>> m_map;
    bool m_has_updated_resources { false };
};

// https://w3c.github.io/ServiceWorker/#update
static void update(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // 1. Let registration be the result of running Get Registration given job’s storage key and job’s scope url.
    auto registration = Registration::get(job->storage_key, job->scope_url);

    // 2. If registration is null, then:
    if (!registration.has_value()) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise<JS::TypeError>(job, "Service Worker registration not found on update"_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 3. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as the argument.
    auto* newest_worker = registration->newest_worker();

    // 4. If job’s job type is update, and newestWorker is not null and its script url does not equal job’s script url, then:
    if (job->job_type == Job::Type::Update && newest_worker != nullptr && newest_worker->script_url != job->script_url) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise<JS::TypeError>(job, "Service Worker script URL mismatch on update"_string);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 5. Let hasUpdatedResources be false.
    // 6. Let updatedResourceMap be an ordered map where the keys are URLs and the values are responses.
    auto state = UpdateAlgorithmState::create(vm);

    // Fetch time, with a few caveats:
    // - The spec says to use the 'to be created environment settings object for this service worker'
    // - Soft-Update has no client

    // To perform the fetch hook given request, run the following steps:
    auto perform_the_fetch_hook_function = [&registration = *registration, job, newest_worker, state](JS::NonnullGCPtr<Fetch::Infrastructure::Request> request, HTML::TopLevelModule top_level, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_custom_fetch_response) -> WebIDL::ExceptionOr<void> {
        // FIXME: Soft-Update has no client
        auto& realm = job->client->realm();
        auto& vm = realm.vm();

        // 1. Append `Service-Worker`/`script` to request’s header list.
        // Note: See https://w3c.github.io/ServiceWorker/#service-worker
        request->header_list()->append(Fetch::Infrastructure::Header::from_string_pair("Service-Worker"sv, "script"sv));

        // 2. Set request’s cache mode to "no-cache" if any of the following are true:
        //  - registration’s update via cache mode is not "all".
        //  - job’s force bypass cache flag is set.
        //  - newestWorker is not null and registration is stale.
        if (registration.update_via_cache() != Bindings::ServiceWorkerUpdateViaCache::All
            || job->force_cache_bypass
            || (newest_worker != nullptr && registration.is_stale())) {
            request->set_cache_mode(Fetch::Infrastructure::Request::CacheMode::NoCache);
        }

        // 3. Set request’s service-workers mode to "none".
        request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        Web::Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
        fetch_algorithms_input.process_response_consume_body = move(process_custom_fetch_response);

        // 4. If the isTopLevel flag is unset, then return the result of fetching request.
        // FIXME: Needs spec issue, this wording is confusing and contradicts the way perform the fetch hook is used in `run a worker`
        if (top_level == HTML::TopLevelModule::No) {
            TRY(Web::Fetch::Fetching::fetch(realm, request, Web::Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
            return {};
        }

        // 5. Set request's redirect mode to "error".
        request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Error);

        // 6. Fetch request, and asynchronously wait to run the remaining steps as part of fetch’s processResponse for the response response.
        // Note: The rest of the steps are in the processCustomFetchResponse algorithm
        // FIXME: Needs spec issue to mention the existence of processCustomFetchResponse, same as step 4

        // FIXME: Is there a better way to 'wait' for the fetch's processResponse to complete?
        //        Is this actually what the spec wants us to do?
        IGNORE_USE_IN_ESCAPING_LAMBDA auto process_response_completion_result = Optional<WebIDL::ExceptionOr<void>> {};

        fetch_algorithms_input.process_response = [request, job, state, newest_worker, &realm, &registration, &process_response_completion_result](JS::NonnullGCPtr<Fetch::Infrastructure::Response> response) mutable -> void {
            // 7. Extract a MIME type from the response’s header list. If s MIME type (ignoring parameters) is not a JavaScript MIME type, then:
            auto mime_type = response->header_list()->extract_mime_type();
            if (!mime_type.has_value() || !mime_type->is_javascript()) {
                // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
                reject_job_promise<WebIDL::SecurityError>(job, "Service Worker script response is not a JavaScript MIME type"_string);

                // 2. Asynchronously complete these steps with a network error.
                process_response_completion_result = WebIDL::NetworkError::create(realm, "Service Worker script response is not a JavaScript MIME type"_string);
                return;
            }

            // 8. Let serviceWorkerAllowed be the result of extracting header list values given `Service-Worker-Allowed` and response’s header list.
            // Note: See the definition of the Service-Worker-Allowed header in Appendix B: Extended HTTP headers. https://w3c.github.io/ServiceWorker/#service-worker-allowed
            auto service_worker_allowed = Fetch::Infrastructure::extract_header_list_values("Service-Worker-Allowed"sv.bytes(), response->header_list());

            // 9. Set policyContainer to the result of creating a policy container from a fetch response given response.
            // FIXME: CSP not implemented yet

            // 10. If serviceWorkerAllowed is failure, then:
            if (service_worker_allowed.has<Fetch::Infrastructure::ExtractHeaderParseFailure>()) {
                // FIXME: Should we reject the job promise with a security error here?

                // 1. Asynchronously complete these steps with a network error.
                process_response_completion_result = WebIDL::NetworkError::create(realm, "Failed to extract Service-Worker-Allowed header from fetch response"_string);
                return;
            }

            // 11. Let scopeURL be registration’s scope url.
            auto const& scope_url = registration.scope_url();

            // 12. Let maxScopeString be null.
            auto max_scope_string = Optional<ByteString> {};

            auto join_paths_with_slash = [](URL::URL const& url) -> ByteString {
                StringBuilder builder;
                builder.append('/');
                for (auto const& component : url.paths()) {
                    builder.append(component);
                    builder.append('/');
                }
                return builder.to_byte_string();
            };

            // 13. If serviceWorkerAllowed is null, then:
            if (service_worker_allowed.has<Empty>()) {
                // 1. Let resolvedScope be the result of parsing "./" using job’s script url as the base URL.
                auto resolved_scope = DOMURL::parse("./"sv, job->script_url);

                // 2. Set maxScopeString to "/", followed by the strings in resolvedScope’s path (including empty strings), separated from each other by "/".
                max_scope_string = join_paths_with_slash(resolved_scope);
            }
            // 14. Else:
            else {
                // 1. Let maxScope be the result of parsing serviceWorkerAllowed using job’s script url as the base URL.
                auto max_scope = DOMURL::parse(service_worker_allowed.get<Vector<ByteBuffer>>()[0], job->script_url);

                // 2. If maxScope’s origin is job’s script url's origin, then:
                if (max_scope.origin().is_same_origin(job->script_url.origin())) {
                    // 1. Set maxScopeString to "/", followed by the strings in maxScope’s path (including empty strings), separated from each other by "/".
                    max_scope_string = join_paths_with_slash(max_scope);
                }
            }

            // 15. Let scopeString be "/", followed by the strings in scopeURL’s path (including empty strings), separated from each other by "/".
            auto scope_string = join_paths_with_slash(scope_url);

            // 16. If maxScopeString is null or scopeString does not start with maxScopeString, then:
            if (!max_scope_string.has_value() || !scope_string.starts_with(max_scope_string.value())) {
                // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
                reject_job_promise<WebIDL::SecurityError>(job, "Service Worker script scope does not match Service-Worker-Allowed header"_string);

                // 2. Asynchronously complete these steps with a network error.
                process_response_completion_result = WebIDL::NetworkError::create(realm, "Service Worker script scope does not match Service-Worker-Allowed header"_string);
                return;
            }

            // 17. Let url be request’s url.
            auto& url = request->url();

            // 18. Set updatedResourceMap[url] to response.
            state->updated_resource_map().set(url, response);

            // 19. If response’s cache state is not "local", set registration’s last update check time to the current time.
            if (response->cache_state() != Fetch::Infrastructure::Response::CacheState::Local)
                registration.set_last_update_check_time(MonotonicTime::now());

            // 20. Set hasUpdatedResources to true if any of the following are true:
            // - newestWorker is null.
            // - newestWorker’s script url is not url or newestWorker’s type is not job’s worker type.
            // - FIXME: newestWorker’s script resource map[url]'s body is not byte-for-byte identical with response’s body.
            if (newest_worker == nullptr
                || newest_worker->script_url != url
                || newest_worker->worker_type != job->worker_type) {
                state->set_has_updated_resources(true);
            }

            // FIXME: 21. If hasUpdatedResources is false and newestWorker’s classic scripts imported flag is set, then:

            // 22. Asynchronously complete these steps with response.
            process_response_completion_result = WebIDL::ExceptionOr<void> {};
        };

        auto fetch_controller = TRY(Web::Fetch::Fetching::fetch(realm, request, Web::Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));

        // FIXME: This feels.. uncomfortable but it should work to block the current task until the fetch has progressed past our processResponse hook or aborted
        auto& event_loop = job->client ? job->client->responsible_event_loop() : HTML::main_thread_event_loop();
        event_loop.spin_until([fetch_controller, &realm, &process_response_completion_result]() -> bool {
            if (process_response_completion_result.has_value())
                return true;
            if (fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Terminated || fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Aborted) {
                process_response_completion_result = WebIDL::AbortError::create(realm, "Service Worker fetch was terminated or aborted"_string);
                return true;
            }
            return false;
        });

        return process_response_completion_result.release_value();
    };
    auto perform_the_fetch_hook = HTML::create_perform_the_fetch_hook(vm.heap(), move(perform_the_fetch_hook_function));

    // When the algorithm asynchronously completes, continue the rest of these steps, with script being the asynchronous completion value.
    auto on_fetch_complete = HTML::create_on_fetch_script_complete(vm.heap(), [job, newest_worker, state, &registration = *registration, &vm](JS::GCPtr<HTML::Script> script) -> void {
        // If script is null or Is Async Module with script’s record, script’s base URL, and « » is true, then:
        // FIXME: Reject async modules
        if (!script) {
            // 1. Invoke Reject Job Promise with job and TypeError.
            reject_job_promise<JS::TypeError>(job, "Service Worker script is not a valid module"_string);

            // 2. If newestWorker is null, then remove registration map[(registration’s storage key, serialized scopeURL)].
            if (newest_worker == nullptr)
                Registration::remove(registration.storage_key(), registration.scope_url());

            // 3. Invoke Finish Job with job and abort these steps.
            finish_job(vm, job);
            return;
        }

        // FIXME: Actually create service worker
        // 10. Let worker be a new service worker.
        // 11. Set worker’s script url to job’s script url, worker’s script resource to script, worker’s type to job’s worker type, and worker’s script resource map to updatedResourceMap.
        (void)state;
        // 12. Append url to worker’s set of used scripts.
        // 13. Set worker’s script resource’s policy container to policyContainer.
        // 14. Let forceBypassCache be true if job’s force bypass cache flag is set, and false otherwise.
        // 15. Let runResult be the result of running the Run Service Worker algorithm with worker and forceBypassCache.
        // 16. If runResult is failure or an abrupt completion, then:
        // 17. Else, invoke Install algorithm with job, worker, and registration as its arguments.
        if (job->client) {
            auto context = HTML::TemporaryExecutionContext(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto& realm = *vm.current_realm();
            WebIDL::reject_promise(realm, *job->job_promise, *vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Run Service Worker"sv).value());
            finish_job(vm, job);
        }
    });

    // 7. Switching on job’s worker type, run these substeps with the following options:
    switch (job->worker_type) {
    case Bindings::WorkerType::Classic:
        // 1. Fetch a classic worker script given job’s serialized script url, job’s client, "serviceworker", and the to-be-created environment settings object for this service worker.
        // FIXME: Credentials mode
        // FIXME: Use a 'stub' service worker ESO as the fetch "environment"
        (void)HTML::fetch_classic_worker_script(job->script_url, *job->client, Fetch::Infrastructure::Request::Destination::ServiceWorker, *job->client, perform_the_fetch_hook, on_fetch_complete);
        break;
    case Bindings::WorkerType::Module:
        // 2. Fetch a module worker script graph given job’s serialized script url, job’s client, "serviceworker", "omit", and the to-be-created environment settings object for this service worker.
        // FIXME: Credentials mode
        // FIXME: Use a 'stub' service worker ESO as the fetch "environment"
        (void)HTML::fetch_module_worker_script_graph(job->script_url, *job->client, Fetch::Infrastructure::Request::Destination::ServiceWorker, *job->client, perform_the_fetch_hook, on_fetch_complete);
    }
}

static void unregister(JS::VM& vm, JS::NonnullGCPtr<Job> job)
{
    // If there's no client, there won't be any promises to resolve
    if (job->client) {
        auto context = HTML::TemporaryExecutionContext(*job->client, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto& realm = *vm.current_realm();
        WebIDL::reject_promise(realm, *job->job_promise, *vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Service Worker unregistration"sv).value());
        finish_job(vm, job);
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
static void reject_job_promise(JS::NonnullGCPtr<Job> job, String message)
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
