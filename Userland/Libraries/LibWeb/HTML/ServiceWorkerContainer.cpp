/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ServiceWorkerContainerPrototype.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/ServiceWorkerContainer.h>
#include <LibWeb/ServiceWorker/Job.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ServiceWorkerContainer);

ServiceWorkerContainer::ServiceWorkerContainer(JS::Realm& realm)
    : DOM::EventTarget(realm)
    , m_service_worker_client(relevant_settings_object(*this))
{
}

ServiceWorkerContainer::~ServiceWorkerContainer() = default;

void ServiceWorkerContainer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ServiceWorkerContainer);
}

void ServiceWorkerContainer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_service_worker_client);
}

JS::NonnullGCPtr<ServiceWorkerContainer> ServiceWorkerContainer::create(JS::Realm& realm)
{
    return realm.heap().allocate<ServiceWorkerContainer>(realm, realm);
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-register
JS::NonnullGCPtr<JS::Promise> ServiceWorkerContainer::register_(String script_url, RegistrationOptions const& options)
{
    auto& realm = this->realm();
    // Note: The register(scriptURL, options) method creates or updates a service worker registration for the given scope url.
    // If successful, a service worker registration ties the provided scriptURL to a scope url,
    // which is subsequently used for navigation matching.

    // 1. Let p be a promise.
    auto p = WebIDL::create_promise(realm);

    // FIXME: 2. Set scriptURL to the result of invoking Get Trusted Type compliant string with TrustedScriptURL,
    //    this's relevant global object, scriptURL, "ServiceWorkerContainer register", and "script".

    // 3 Let client be this's service worker client.
    auto client = m_service_worker_client;

    // 4. Let scriptURL be the result of parsing scriptURL with this's relevant settings object’s API base URL.
    auto base_url = relevant_settings_object(*this).api_base_url();
    auto parsed_script_url = DOMURL::parse(script_url, base_url);

    // 5. Let scopeURL be null.
    Optional<URL::URL> scope_url;

    // 6. If options["scope"] exists, set scopeURL to the result of parsing options["scope"] with this's relevant settings object’s API base URL.
    if (options.scope.has_value()) {
        scope_url = DOMURL::parse(options.scope.value(), base_url);
    }

    // 7. Invoke Start Register with scopeURL, scriptURL, p, client, client’s creation URL, options["type"], and options["updateViaCache"].
    start_register(scope_url, parsed_script_url, p, client, client->creation_url, options.type, options.update_via_cache);

    // 8. Return p.
    return verify_cast<JS::Promise>(*p->promise());
}

// https://w3c.github.io/ServiceWorker/#start-register-algorithm
void ServiceWorkerContainer::start_register(Optional<URL::URL> scope_url, URL::URL script_url, JS::NonnullGCPtr<WebIDL::Promise> promise, EnvironmentSettingsObject& client, URL::URL referrer, Bindings::WorkerType worker_type, Bindings::ServiceWorkerUpdateViaCache update_via_cache)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. If scriptURL is failure, reject promise with a TypeError and abort these steps.
    if (!script_url.is_valid()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "scriptURL is not a valid URL"sv));
        return;
    }

    // 2. Set scriptURL’s fragment to null.
    // Note:  The user agent does not store the fragment of the script’s url.
    //        This means that the fragment does not have an effect on identifying service workers.
    script_url.set_fragment({});

    // 3. If scriptURL’s scheme is not one of "http" and "https", reject promise with a TypeError and abort these steps.
    if (!script_url.scheme().is_one_of("http"sv, "https"sv)) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "scriptURL must have a scheme of 'http' or 'https'"sv));
        return;
    }

    // 4. If any of the strings in scriptURL’s path contains either ASCII case-insensitive "%2f" or ASCII case-insensitive "%5c",
    //    reject promise with a TypeError and abort these steps.
    auto invalid_path = script_url.paths().first_matching([&](auto& path) {
        return path.contains("%2f"sv, CaseSensitivity::CaseInsensitive) || path.contains("%5c"sv, CaseSensitivity::CaseInsensitive);
    });
    if (invalid_path.has_value()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "scriptURL path must not contain '%2f' or '%5c'"sv));
        return;
    }

    // 5. If scopeURL is null, set scopeURL to the result of parsing the string "./" with scriptURL.
    // Note: The scope url for the registration is set to the location of the service worker script by default.
    if (!scope_url.has_value()) {
        scope_url = DOMURL::parse("./"sv, script_url);
    }

    // 6. If scopeURL is failure, reject promise with a TypeError and abort these steps.
    if (!scope_url->is_valid()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "scopeURL is not a valid URL"sv));
        return;
    }

    // 7. Set scopeURL’s fragment to null.
    // Note: The user agent does not store the fragment of the scope url.
    //       This means that the fragment does not have an effect on identifying service worker registrations.
    scope_url->set_fragment({});

    // 8. If scopeURL’s scheme is not one of "http" and "https", reject promise with a TypeError and abort these steps.
    if (!scope_url->scheme().is_one_of("http"sv, "https"sv)) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "scopeURL must have a scheme of 'http' or 'https'"sv));
        return;
    }

    // 9. If any of the strings in scopeURL’s path contains either ASCII case-insensitive "%2f" or ASCII case-insensitive "%5c",
    //    reject promise with a TypeError and abort these steps.
    invalid_path = scope_url->paths().first_matching([&](auto& path) {
        return path.contains("%2f"sv, CaseSensitivity::CaseInsensitive) || path.contains("%5c"sv, CaseSensitivity::CaseInsensitive);
    });
    if (invalid_path.has_value()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "scopeURL path must not contain '%2f' or '%5c'"sv));
        return;
    }

    // 10. Let storage key be the result of running obtain a storage key given client.
    auto storage_key = StorageAPI::obtain_a_storage_key(client);

    // FIXME: Ad-Hoc. Spec should handle this failure here, or earlier.
    if (!storage_key.has_value()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to obtain a storage key"sv));
        return;
    }

    // 11. Let job be the result of running Create Job with register, storage key, scopeURL, scriptURL, promise, and client.
    auto job = ServiceWorker::Job::create(vm, ServiceWorker::Job::Type::Register, storage_key.value(), scope_url.value(), script_url, promise, client);

    // 12. Set job’s worker type to workerType.
    job->worker_type = worker_type;

    // 13. Set job’s update via cache to updateViaCache.
    job->update_via_cache = update_via_cache;

    // 14. Set job’s referrer to referrer.
    job->referrer = move(referrer);

    // 15. Invoke Schedule Job with job.
    ServiceWorker::schedule_job(vm, job);
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                    \
    void ServiceWorkerContainer::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                              \
        set_event_handler_attribute(event_name, move(value));                      \
    }                                                                              \
    WebIDL::CallbackType* ServiceWorkerContainer::attribute_name()                 \
    {                                                                              \
        return event_handler_attribute(event_name);                                \
    }
ENUMERATE_SERVICE_WORKER_CONTAINER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
