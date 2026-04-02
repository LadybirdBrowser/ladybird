/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/CachePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(Cache);
GC_DEFINE_ALLOCATOR(CacheBatchOperation);

Cache::Cache(JS::Realm& realm, GC::Ref<RequestResponseList> request_response_list)
    : Bindings::PlatformObject(realm)
    , m_request_response_list(request_response_list)
{
}

void Cache::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Cache);
}

void Cache::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request_response_list);
}

// https://w3c.github.io/ServiceWorker/#cache-match
GC::Ref<WebIDL::Promise> Cache::match(Fetch::RequestInfo request, CacheQueryOptions options)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise, request = move(request), options]() {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let p be the result of running the algorithm specified in matchAll(request, options) method with request and options.
        // 2. Wait until p settles.
        WebIDL::react_to_promise(match_all(move(request), options),
            // 4. Else if p resolves with an array, responses, then:
            GC::create_function(realm.heap(), [&realm, promise](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If responses is an empty array, then:
                if (auto& responses = value.as<JS::Array>(); responses.indexed_array_like_size() == 0) {
                    // 1. Resolve promise with undefined.
                    WebIDL::resolve_promise(realm, promise, JS::js_undefined());
                }
                // 2. Else:
                else {
                    // 1. Resolve promise with the first element of responses.
                    auto first_element = responses.indexed_get(0).release_value();
                    WebIDL::resolve_promise(realm, promise, first_element.value);
                }

                return JS::js_undefined();
            }),

            // 3. If p rejects with an exception, then:
            GC::create_function(realm.heap(), [&realm, promise](JS::Value exception) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. Reject promise with that exception.
                WebIDL::reject_promise(realm, promise, exception);

                return JS::js_undefined();
            }));
    }));

    // 3. Return promise.
    return promise;
}

// https://w3c.github.io/ServiceWorker/#cache-matchall
GC::Ref<WebIDL::Promise> Cache::match_all(Optional<Fetch::RequestInfo> request, CacheQueryOptions options)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let r be null.
    GC::Ptr<Fetch::Infrastructure::Request> inner_request;

    // 2. If the optional argument request is not omitted, then:
    if (request.has_value()) {
        TRY(request->visit(
            // 1. If request is a Request object, then:
            [&](GC::Root<Fetch::Request> const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
                // 1. Set r to request’s request.
                inner_request = request->request();

                // 2. If r’s method is not `GET` and options.ignoreMethod is false, return a promise resolved with an
                //    empty array.
                if (inner_request->method() != "GET"sv && !options.ignore_method)
                    return WebIDL::create_resolved_promise(realm, MUST(JS::Array::create(realm, 0)));

                return {};
            },
            // 2. Else if request is a string, then:
            [&](String const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
                // 1. Set r to the associated request of the result of invoking the initial value of Request as
                //    constructor with request as its argument. If this throws an exception, return a promise rejected
                //    with that exception.
                auto request_object = Fetch::Request::construct_impl(realm, request);
                if (request_object.is_error())
                    return WebIDL::create_rejected_promise_from_exception(realm, request_object.release_error());

                inner_request = request_object.value()->request();
                return {};
            }));
    }

    // 3. Let realm be this’s relevant realm.
    // 4. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 5. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, inner_request, promise, request = move(request), options]() {
        // 1. Let responses be an empty list.
        auto responses = realm.heap().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::Response>>>();

        // 2. If the optional argument request is omitted, then:
        if (!request.has_value()) {
            // 1. For each requestResponse of the relevant request response list:
            for (auto& request_response : m_request_response_list->elements()) {
                // 1. Add a copy of requestResponse’s response to responses.
                responses->elements().append(request_response->response->clone(realm));
            }
        }
        // 3. Else:
        else {
            // 1. Let requestResponses be the result of running Query Cache with r and options.
            auto request_responses = query_cache(*inner_request, options);

            // 2. For each requestResponse of requestResponses:
            for (auto request_response : request_responses->elements()) {
                // 1. Add a copy of requestResponse’s response to responses.
                // NB: No need to copy. Query Cache creates a copy, and the requestResponses list is dropped hereafter.
                responses->elements().append(request_response->response);
            }
        }

        // 3. For each response of responses:
        for (auto response : responses->elements()) {
            // 1. If response’s type is "opaque" and cross-origin resource policy check with promise’s relevant settings
            //    object’s origin, promise’s relevant settings object, "", and response’s internal response returns
            //    blocked, then reject promise with a TypeError and abort these steps.
            if (response->type() == Fetch::Infrastructure::Response::Type::Opaque) {
                // FIXME: Perform the cross-origin resource policy check.
            }
        }

        // 4. Queue a task, on promise’s relevant settings object’s responsible event loop using the DOM manipulation
        //    task source, to perform the following steps:
        HTML::queue_a_task(
            HTML::Task::Source::DOMManipulation,
            HTML::relevant_settings_object(promise->promise()).responsible_event_loop(),
            {},
            GC::create_function(realm.heap(), [&realm, promise, responses]() {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. Let responseList be a list.
                auto response_list = realm.heap().allocate<GC::HeapVector<JS::Value>>();

                // 2. For each response of responses:
                for (auto response : responses->elements()) {
                    // 1. Add a new Response object associated with response and a new Headers object whose guard is
                    //    "immutable" to responseList.
                    response_list->elements().append(Fetch::Response::create(realm, response, Fetch::Headers::Guard::Immutable));
                }

                // 3. Resolve promise with a frozen array created from responseList, in realm.
                WebIDL::resolve_promise(realm, promise, JS::Array::create_from(realm, response_list->elements()));
            }));
    }));

    // 6. Return promise.
    return promise;
}

// https://w3c.github.io/ServiceWorker/#cache-add
GC::Ref<WebIDL::Promise> Cache::add(Fetch::RequestInfo request)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let requests be an array containing only request.
    // 2. Let responseArrayPromise be the result of running the algorithm specified in addAll(requests) passing requests
    //    as the argument.
    auto promise = add_all({ { request } });

    // 3. Return the result of reacting to responseArrayPromise with a fulfillment handler that returns undefined.
    return WebIDL::upon_fulfillment(promise, GC::create_function(realm.heap(), [](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-addAll
GC::Ref<WebIDL::Promise> Cache::add_all(ReadonlySpan<Fetch::RequestInfo> requests)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let responsePromises be an empty list.
    auto response_promises = realm.heap().allocate<GC::HeapVector<GC::Ref<WebIDL::Promise>>>();

    // 2. Let requestList be an empty list.
    auto request_list = realm.heap().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::Request>>>();

    // 3. For each request whose type is Request in requests:
    for (auto const& request_info : requests) {
        if (auto const* request = request_info.get_pointer<GC::Root<Fetch::Request>>()) {
            // 1. Let r be request’s request.
            auto inner_request = (*request)->request();

            // 2. If r’s url’s scheme is not one of "http" and "https", or r’s method is not `GET`, return a promise
            //    rejected with a TypeError.
            if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv) || inner_request->method() != "GET"sv)
                return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Request must be a GET request with an HTTP(S) URL"sv));
        }
    }

    // 4. Let fetchControllers be a list of fetch controllers.
    auto fetch_controllers = realm.heap().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::FetchController>>>();

    // 5. For each request in requests:
    for (auto const& request_info : requests) {
        // 1. Let r be the associated request of the result of invoking the initial value of Request as constructor with
        //    request as its argument. If this throws an exception, return a promise rejected with that exception.
        auto result = Fetch::Request::construct_impl(realm, request_info);
        if (result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());

        auto inner_request = result.value()->request();

        // 2. If r’s url’s scheme is not one of "http" and "https", then:
        if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv)) {
            // 1. For each fetchController of fetchControllers, abort fetchController.
            for (auto fetch_controller : fetch_controllers->elements())
                fetch_controller->abort(realm, {});

            // 2. Return a promise rejected with a TypeError.
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Request must have an HTTP(S) URL"sv));
        }

        // 3. If r’s client’s global object is a ServiceWorkerGlobalScope object, set request’s service-workers mode to "none".
        if (is<ServiceWorkerGlobalScope>(inner_request->client()->global_object()))
            inner_request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        // 4. Set r’s initiator to "fetch" and destination to "subresource".
        // FIXME: Spec issue: There is no "fetch" initiator (spec probably wants initiator type). And there is no
        //        "subresource" destination (so we set it to the "empty string" destination for now).
        //        https://github.com/w3c/ServiceWorker/issues/1718
        inner_request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Fetch);
        inner_request->set_destination({});

        // 5. Add r to requestList.
        request_list->elements().append(inner_request);

        // 6. Let responsePromise be a new promise.
        auto response_promise = WebIDL::create_promise(realm);

        // 7. Run the following substeps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, fetch_controllers, inner_request, response_promise]() {
            // * Append the result of fetching r.
            Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};

            // * To processResponse for response, run these substeps:
            fetch_algorithms_input.process_response = [&realm, fetch_controllers, response_promise](GC::Ref<Fetch::Infrastructure::Response> response) {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                bool did_reject_promise = false;

                // 1. If response’s type is "error", or response’s status is not an ok status or is 206, reject
                //    responsePromise with a TypeError.
                auto status = response->status();
                if (response->type() == Fetch::Infrastructure::Response::Type::Error || !Fetch::Infrastructure::is_ok_status(status) || status == 206) {
                    WebIDL::reject_promise(realm, response_promise, JS::TypeError::create(realm, "Fetch request failed"sv));
                    did_reject_promise = true;
                }
                // 2. Else if response’s header list contains a header named `Vary`, then:
                else if (response->header_list()->contains("Vary"sv)) {
                    // 1. Let fieldValues be the list containing the elements corresponding to the field-values of the
                    //    Vary header.
                    // 2. For each fieldValue of fieldValues:
                    response->header_list()->for_each_vary_header([&](StringView field_value) {
                        // 1. If fieldValue matches "*", then:
                        if (field_value == "*"sv) {
                            // 1. Reject responsePromise with a TypeError.
                            WebIDL::reject_promise(realm, response_promise, JS::TypeError::create(realm, "Vary '*' is not supported"sv));
                            did_reject_promise = true;

                            // 2. For each fetchController of fetchControllers, abort fetchController.
                            for (auto fetch_controller : fetch_controllers->elements())
                                fetch_controller->abort(realm, {});

                            // 3. Abort these steps.
                            return IterationDecision::Break;
                        }

                        return IterationDecision::Continue;
                    });
                }

                if (did_reject_promise)
                    return;

                // * To processResponseEndOfBody for response, run these substeps:
                // FIXME: Spec issue? processResponseEndOfBody is not invoked until the response's body is read, but
                //        doing so here locks the body's stream, thus cloning the response during batch operations fails.
                //        So we resolve the pending promise here, which seems to work fine.

                // 1. If response’s aborted flag is set, reject responsePromise with an "AbortError" DOMException and
                //    abort these steps.
                if (response->aborted()) {
                    WebIDL::reject_promise(realm, response_promise, WebIDL::AbortError::create(realm, "Fetch request was aborted"_utf16));
                    return;
                }

                // 2. Resolve responsePromise with response.
                WebIDL::resolve_promise(realm, response_promise, response);
            };

            auto fetch_controller = Fetch::Fetching::fetch(realm, inner_request, Fetch::Infrastructure::FetchAlgorithms::create(realm.vm(), move(fetch_algorithms_input)));
            fetch_controllers->elements().append(fetch_controller);

            // Note: The cache commit is allowed when the response’s body is fully received.
        }));

        // 8. Add responsePromise to responsePromises.
        response_promises->elements().append(response_promise);
    }

    // 6. Let p be the result of getting a promise to wait for all of responsePromises.
    auto promise = WebIDL::get_promise_for_wait_for_all(realm, response_promises->elements());

    // 7. Return the result of reacting to p with a fulfillment handler that, when called with argument responses, performs the following substeps:
    return WebIDL::upon_fulfillment(promise, GC::create_function(realm.heap(), [this, &realm, request_list](JS::Value result) -> WebIDL::ExceptionOr<JS::Value> {
        HTML::TemporaryExecutionContext context { realm };
        auto& responses = result.as<JS::Array>();

        // 1. Let operations be an empty list.
        auto operations = realm.heap().allocate<GC::HeapVector<GC::Ref<CacheBatchOperation>>>();

        // 2. Let index be zero.
        // 3. For each response in responses:
        for (size_t index = 0; index < responses.indexed_array_like_size(); ++index) {
            auto& response = as<Fetch::Infrastructure::Response>(responses.indexed_get(index)->value.as_cell());

            // 1. Let operation be a cache batch operation.
            auto operation = realm.heap().allocate<CacheBatchOperation>(
                // 2. Set operation’s type to "put".
                CacheBatchOperation::Type::Put,
                // 3. Set operation’s request to requestList[index].
                request_list->elements()[index],
                // 4. Set operation’s response to response.
                response);

            // 5. Append operation to operations.
            operations->elements().append(operation);

            // 6. Increment index by one.
        }

        // 4. Let realm be this’s relevant realm.
        // 5. Let cacheJobPromise be a new promise.
        auto cache_job_promise = WebIDL::create_promise(realm);

        // 6. Run the following substeps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, operations, cache_job_promise]() {
            // 1. Let errorData be null.
            // 2. Invoke Batch Cache Operations with operations. If this throws an exception, set errorData to the
            //    exception.
            auto error_data = batch_cache_operations(operations);

            // 3. Queue a task, on cacheJobPromise’s relevant settings object’s responsible event loop using the DOM
            //    manipulation task source, to perform the following substeps:
            HTML::queue_a_task(
                HTML::Task::Source::DOMManipulation,
                HTML::relevant_settings_object(cache_job_promise->promise()).responsible_event_loop(),
                {},
                GC::create_function(realm.heap(), [&realm, cache_job_promise, error_data = move(error_data)]() mutable {
                    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    // 1. If errorData is null, resolve cacheJobPromise with undefined.
                    if (!error_data.is_error())
                        WebIDL::resolve_promise(realm, cache_job_promise, JS::js_undefined());
                    // 2. Else, reject cacheJobPromise with a new exception with errorData, in realm.
                    else
                        WebIDL::reject_promise_with_exception(realm, cache_job_promise, error_data.release_error());
                }));
        }));

        // 7. Return cacheJobPromise.
        return cache_job_promise->promise();
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-put
GC::Ref<WebIDL::Promise> Cache::put(Fetch::RequestInfo request, GC::Ref<Fetch::Response> response)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let innerRequest be null.
    GC::Ptr<Fetch::Infrastructure::Request> inner_request;

    TRY(request.visit(
        // 2. If request is a Request object, then set innerRequest to request’s request.
        [&](GC::Root<Fetch::Request> const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
            inner_request = request->request();
            return {};
        },
        // 3. Else:
        [&](String const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
            // 1. Let requestObj be the result of invoking Request’s constructor with request as its argument. If this
            //    throws an exception, return a promise rejected with exception.
            auto request_object = Fetch::Request::construct_impl(realm, request);
            if (request_object.is_error())
                return WebIDL::create_rejected_promise_from_exception(realm, request_object.release_error());

            inner_request = request_object.value()->request();
            return {};
        }));

    // 4. If innerRequest’s url’s scheme is not one of "http" and "https", or innerRequest’s method is not `GET`, return
    //    a promise rejected with a TypeError.
    if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv) || inner_request->method() != "GET"sv)
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Request must be a GET request with an HTTP(S) URL"sv));

    // 5. Let innerResponse be response’s response.
    auto inner_response = response->response();

    // 6. If innerResponse’s status is 206, return a promise rejected with a TypeError.
    if (inner_response->status() == 206)
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Partial responses are not supported"sv));

    // 7. If innerResponse’s header list contains a header named `Vary`, then:
    //     1. Let fieldValues be the list containing the items corresponding to the Vary header’s field-values.
    //     2. For each fieldValue in fieldValues:
    bool found_vary_wildcard = false;

    inner_response->header_list()->for_each_vary_header([&](StringView field_value) {
        // 1. If fieldValue matches "*", return a promise rejected with a TypeError.
        found_vary_wildcard = field_value == "*"sv;
        return found_vary_wildcard ? IterationDecision::Break : IterationDecision::Continue;
    });

    if (found_vary_wildcard)
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Vary '*' is not supported"sv));

    // 8. If innerResponse’s body is disturbed or locked, return a promise rejected with a TypeError.
    if (auto body = inner_response->body()) {
        if (auto stream = body->stream(); stream->is_disturbed() || stream->is_locked())
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Response's body stream is disturbed or locked"sv));
    }

    // 9. Let clonedResponse be a clone of innerResponse.
    auto cloned_response = inner_response->clone(realm);

    // 10. Let bodyReadPromise be a promise resolved with undefined.
    auto body_read_promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 11. If innerResponse’s body is non-null, run these substeps:
    if (auto body = inner_response->body()) {
        // 1. Let stream be innerResponse’s body’s stream.
        auto stream = body->stream();

        // 2. Let reader be the result of getting a reader for stream.
        auto reader = MUST(stream->get_a_reader());

        // 3. Set bodyReadPromise to the result of reading all bytes from reader.
        body_read_promise = reader->read_all_bytes_deprecated();
    }

    // Note: This ensures that innerResponse’s body is locked, and we have a full buffered copy of the body in
    //       clonedResponse. An implementation could optimize by streaming directly to disk rather than memory.

    // 12. Let operations be an empty list.
    auto operations = realm.heap().allocate<GC::HeapVector<GC::Ref<CacheBatchOperation>>>();

    // 13. Let operation be a cache batch operation.
    auto operation = realm.heap().allocate<CacheBatchOperation>(
        // 14. Set operation’s type to "put".
        CacheBatchOperation::Type::Put,
        // 15. Set operation’s request to innerRequest.
        *inner_request,
        // 16. Set operation’s response to clonedResponse.
        cloned_response);

    // 17. Append operation to operations.
    operations->elements().append(operation);

    // 18. Let realm be this’s relevant realm.
    // 19. Return the result of the fulfillment of bodyReadPromise:
    return WebIDL::upon_fulfillment(body_read_promise, GC::create_function(realm.heap(), [this, &realm, operations](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        HTML::TemporaryExecutionContext context { realm };

        // 1. Let cacheJobPromise be a new promise.
        auto cache_job_promise = WebIDL::create_promise(realm);

        // 2. Return cacheJobPromise and run these steps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, operations, cache_job_promise]() {
            // 1. Let errorData be null.
            // 2. Invoke Batch Cache Operations with operations. If this throws an exception, set errorData to the exception.
            auto error_data = batch_cache_operations(operations);

            // 3. Queue a task, on cacheJobPromise’s relevant settings object’s responsible event loop using the DOM
            //    manipulation task source, to perform the following substeps:
            HTML::queue_a_task(
                HTML::Task::Source::DOMManipulation,
                HTML::relevant_settings_object(cache_job_promise->promise()).responsible_event_loop(),
                {},
                GC::create_function(realm.heap(), [&realm, cache_job_promise, error_data = move(error_data)]() mutable {
                    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    // 1. If errorData is null, resolve cacheJobPromise with undefined.
                    if (!error_data.is_error())
                        WebIDL::resolve_promise(realm, cache_job_promise, JS::js_undefined());
                    // 2. Else, reject cacheJobPromise with a new exception with errorData, in realm.
                    else
                        WebIDL::reject_promise_with_exception(realm, cache_job_promise, error_data.release_error());
                }));
        }));

        return cache_job_promise->promise();
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-delete
GC::Ref<WebIDL::Promise> Cache::delete_(Fetch::RequestInfo request, CacheQueryOptions options)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let r be null.
    GC::Ptr<Fetch::Infrastructure::Request> inner_request;

    TRY(request.visit(
        // 2. If request is a Request object, then:
        [&](GC::Root<Fetch::Request> const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
            // 1. Set r to request’s request.
            inner_request = request->request();

            // 2. If r’s method is not `GET` and options.ignoreMethod is false, return a promise resolved with false.
            if (inner_request->method() != "GET"sv && !options.ignore_method)
                return WebIDL::create_resolved_promise(realm, JS::Value { false });

            return {};
        },
        // 3. Else if request is a string, then:
        [&](String const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
            // 1. Set r to the associated request of the result of invoking the initial value of Request as constructor
            //    with request as its argument. If this throws an exception, return a promise rejected with that
            //    exception.
            auto request_object = Fetch::Request::construct_impl(realm, request);
            if (request_object.is_error())
                return WebIDL::create_rejected_promise_from_exception(realm, request_object.release_error());

            inner_request = request_object.value()->request();
            return {};
        }));

    // 4. Let operations be an empty list.
    auto operations = realm.heap().allocate<GC::HeapVector<GC::Ref<CacheBatchOperation>>>();

    // 5. Let operation be a cache batch operation.
    auto operation = realm.heap().allocate<CacheBatchOperation>(
        // 6. Set operation’s type to "delete".
        CacheBatchOperation::Type::Delete,
        // 7. Set operation’s request to r.
        *inner_request,
        nullptr,
        // 8. Set operation’s options to options.
        options);

    // 9. Append operation to operations.
    operations->elements().append(operation);

    // 10. Let realm be this’s relevant realm.
    // 11. Let cacheJobPromise be a new promise.
    auto cache_job_promise = WebIDL::create_promise(realm);

    // 12. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, operations, cache_job_promise]() {
        // 1. Let errorData be null.
        // 2. Let requestResponses be the result of running Batch Cache Operations with operations. If this throws an
        //    exception, set errorData to the exception.
        auto error_data = batch_cache_operations(operations);

        // 3. Queue a task, on cacheJobPromise’s relevant settings object’s responsible event loop using the DOM
        //    manipulation task source, to perform the following substeps:
        HTML::queue_a_task(
            HTML::Task::Source::DOMManipulation,
            HTML::relevant_settings_object(cache_job_promise->promise()).responsible_event_loop(),
            {},
            GC::create_function(realm.heap(), [&realm, cache_job_promise, error_data = move(error_data)]() mutable {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If errorData is null, then:
                if (!error_data.is_error()) {
                    // 1. If requestResponses is not empty, resolve cacheJobPromise with true.
                    // 2. Else, resolve cacheJobPromise with false.
                    WebIDL::resolve_promise(realm, cache_job_promise, JS::Value { error_data.value() });
                }
                // 2. Else, reject cacheJobPromise with a new exception with errorData, in realm.
                else {
                    WebIDL::reject_promise_with_exception(realm, cache_job_promise, error_data.release_error());
                }
            }));
    }));

    // 13. Return cacheJobPromise.
    return cache_job_promise;
}

// https://w3c.github.io/ServiceWorker/#cache-keys
GC::Ref<WebIDL::Promise> Cache::keys(Optional<Fetch::RequestInfo> request, CacheQueryOptions options)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let r be null.
    GC::Ptr<Fetch::Infrastructure::Request> inner_request;

    // 2. If the optional argument request is not omitted, then:
    if (request.has_value()) {
        TRY(request->visit(
            // 1. If request is a Request object, then:
            [&](GC::Root<Fetch::Request> const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
                // 1. Set r to request’s request.
                inner_request = request->request();

                // 2. If r’s method is not `GET` and options.ignoreMethod is false, return a promise resolved with an
                //    empty array.
                if (inner_request->method() != "GET"sv && !options.ignore_method)
                    return WebIDL::create_resolved_promise(realm, MUST(JS::Array::create(realm, 0)));

                return {};
            },
            // 2. Else if request is a string, then:
            [&](String const& request) -> ErrorOr<void, GC::Ref<WebIDL::Promise>> {
                // 1. Set r to the associated request of the result of invoking the initial value of Request as
                //    constructor with request as its argument. If this throws an exception, return a promise rejected
                //    with that exception.
                auto request_object = Fetch::Request::construct_impl(realm, request);
                if (request_object.is_error())
                    return WebIDL::create_rejected_promise_from_exception(realm, request_object.release_error());

                inner_request = request_object.value()->request();
                return {};
            }));
    }

    // 3. Let realm be this’s relevant realm.
    // 4. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 5. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, inner_request, promise, request = move(request), options]() {
        // 1. Let requests be an empty list.
        auto requests = realm.heap().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::Request>>>();

        // 2. If the optional argument request is omitted, then:
        if (!request.has_value()) {
            // 1. For each requestResponse of the relevant request response list:
            for (auto& request_response : m_request_response_list->elements()) {
                // 1. Add requestResponse’s request to requests.
                requests->elements().append(request_response->request);
            }
        }
        // 3. Else:
        else {
            // 1. Let requestResponses be the result of running Query Cache with r and options.
            auto request_responses = query_cache(*inner_request, options);

            // 2. For each requestResponse of requestResponses:
            for (auto request_response : request_responses->elements()) {
                // 1. Add requestResponse’s request to requests.
                requests->elements().append(request_response->request);
            }
        }

        // 4. Queue a task, on promise’s relevant settings object’s responsible event loop using the DOM manipulation
        //    task source, to perform the following steps:
        HTML::queue_a_task(
            HTML::Task::Source::DOMManipulation,
            HTML::relevant_settings_object(promise->promise()).responsible_event_loop(),
            {},
            GC::create_function(realm.heap(), [&realm, promise, requests]() {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. Let requestList be a list.
                auto request_list = realm.heap().allocate<GC::HeapVector<JS::Value>>();

                // 2. For each request of requests:
                for (auto request : requests->elements()) {
                    // 1. Add a new Request object associated with request and a new associated Headers object whose
                    //    guard is "immutable" to requestList.
                    request_list->elements().append(Fetch::Request::create(realm, request, Fetch::Headers::Guard::Immutable, MUST(DOM::AbortSignal::construct_impl(realm))));
                }

                // 3. Resolve promise with a frozen array created from requestList, in realm.
                WebIDL::resolve_promise(realm, promise, JS::Array::create_from(realm, request_list->elements()));
            }));
    }));

    // 6. Return promise.
    return promise;
}

// https://w3c.github.io/ServiceWorker/#request-matches-cached-item-algorithm
static bool request_matches_cached_item(
    GC::Ref<Fetch::Infrastructure::Request> request_query,
    GC::Ref<Fetch::Infrastructure::Request> request,
    GC::Ptr<Fetch::Infrastructure::Response> response,
    CacheQueryOptions options)
{
    // 1. If options["ignoreMethod"] is false and request’s method is not `GET`, return false.
    if (!options.ignore_method && request->method() != "GET"sv)
        return false;

    // 2. Let queryURL be requestQuery’s url.
    auto query_url = request_query->url();

    // 3. Let cachedURL be request’s url.
    auto cached_url = request->url();

    // 4. If options["ignoreSearch"] is true, then:
    if (options.ignore_search) {
        // 1. Set cachedURL’s query to the empty string.
        cached_url.set_query(String {});

        // 2. Set queryURL’s query to the empty string.
        query_url.set_query(String {});
    }

    // 5. If queryURL does not equal cachedURL with the exclude fragment flag set, then return false.
    if (!query_url.equals(cached_url, URL::ExcludeFragment::Yes))
        return false;

    // 6. If response is null, options["ignoreVary"] is true, or response’s header list does not contain `Vary`, then
    //    return true.
    if (!response || options.ignore_vary || !response->header_list()->contains("Vary"sv))
        return true;

    // 7. Let fieldValues be the list containing the elements corresponding to the field-values of the Vary header for
    //    the value of the header with name `Vary`.
    // 8. For each fieldValue in fieldValues:
    bool matches = true;

    response->header_list()->for_each_vary_header([&](StringView field_value) {
        // 1. If fieldValue matches "*", or the combined value given fieldValue and request’s header list does not match
        //    the combined value given fieldValue and requestQuery’s header list, then return false.
        matches = field_value == "*"sv
            || request->header_list()->get(field_value) == request_query->header_list()->get(field_value);
        return matches ? IterationDecision::Break : IterationDecision::Continue;
    });

    // 9. Return true.
    return matches;
}

// https://w3c.github.io/ServiceWorker/#query-cache-algorithm
GC::Ref<RequestResponseList> Cache::query_cache(GC::Ref<Fetch::Infrastructure::Request> request_query, CacheQueryOptions options, GC::Ptr<RequestResponseList> target_storage, CloneCache clone_cache)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let resultList be an empty list.
    auto result_list = realm.heap().allocate<RequestResponseList>();

    // 2. Let storage be null.
    // 3. If the optional argument targetStorage is omitted, set storage to the relevant request response list.
    // 4. Else, set storage to targetStorage.
    auto& storage = target_storage ? *target_storage : *m_request_response_list;

    // 5. For each requestResponse of storage:
    for (auto request_response : storage.elements()) {
        // 1. Let cachedRequest be requestResponse’s request.
        auto cached_request = request_response->request;

        // 2. Let cachedResponse be requestResponse’s response.
        auto cached_response = request_response->response;

        // 3. If Request Matches Cached Item with requestQuery, cachedRequest, cachedResponse, and options returns true, then:
        if (request_matches_cached_item(request_query, cached_request, cached_response, options)) {
            // AD-HOC: Not all callers actually need a copy. In Batch Cache Operations, it becomes much easier to remove
            //         matching items from the cache if we can do a pointer comparison.
            if (clone_cache == CloneCache::No) {
                result_list->elements().append(request_response);
                continue;
            }

            // 1. Let requestCopy be a copy of cachedRequest.
            auto request_copy = cached_request->clone(realm);

            // 2. Let responseCopy be a copy of cachedResponse.
            auto response_copy = cached_response->clone(realm);

            // 3. Add requestCopy/responseCopy to resultList.
            result_list->elements().append(realm.heap().allocate<RequestResponse>(request_copy, response_copy));
        }
    }

    // 6. Return resultList.
    return result_list;
}

// https://w3c.github.io/ServiceWorker/#batch-cache-operations-algorithm
WebIDL::ExceptionOr<bool> Cache::batch_cache_operations(GC::Ref<GC::HeapVector<GC::Ref<CacheBatchOperation>>> operations)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let cache be the relevant request response list.
    auto cache = m_request_response_list;

    // 2. Let backupCache be a new request response list that is a copy of cache.
    auto backup_cache = realm.heap().allocate<RequestResponseList>();

    for (auto request_response : cache->elements()) {
        auto backup_request = request_response->request->clone(realm);
        auto backup_response = request_response->response->clone(realm);

        auto backup_request_response = realm.heap().allocate<RequestResponse>(backup_request, backup_response);
        backup_cache->elements().append(backup_request_response);
    }

    // 3. Let addedItems be an empty list.
    auto added_items = realm.heap().allocate<RequestResponseList>();

    // 4. Try running the following substeps atomically:
    auto result = [&]() -> WebIDL::ExceptionOr<bool> {
        // 1. Let resultList be an empty list.
        // NB: This result list is unused, only Cache.delete needs to know if there were any results.
        bool removed_any_items = false;

        auto remove_items_from_cache = [&](auto request_responses) {
            removed_any_items |= cache->elements().remove_all_matching([&](GC::Ref<RequestResponse> request_response) {
                return request_responses->elements().contains_slow(request_response);
            });
        };

        // 2. For each operation in operations:
        for (auto operation : operations->elements()) {
            // 1. If operation’s type matches neither "delete" nor "put", throw a TypeError.
            if (operation->type != CacheBatchOperation::Type::Delete && operation->type != CacheBatchOperation::Type::Put)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Batch operation type must be 'delete' or 'put'"sv };

            // 2. If operation’s type matches "delete" and operation’s response is not null, throw a TypeError.
            if (operation->type == CacheBatchOperation::Type::Delete && operation->response)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Batch operations of type 'delete' must not have a response"sv };

            // 3. If the result of running Query Cache with operation’s request, operation’s options, and addedItems is
            //    not empty, throw an "InvalidStateError" DOMException.
            if (!query_cache(operation->request, operation->options, added_items, CloneCache::No)->elements().is_empty())
                return WebIDL::InvalidStateError::create(realm, "Batch operation requests must be unique"_utf16);

            // 4. Let requestResponses be an empty list.
            GC::Ptr<RequestResponseList> request_responses;

            // 5. If operation’s type matches "delete", then:
            if (operation->type == CacheBatchOperation::Type::Delete) {
                // 1. Set requestResponses to the result of running Query Cache with operation’s request and operation’s options.
                request_responses = query_cache(operation->request, operation->options, {}, CloneCache::No);

                // 2. For each requestResponse in requestResponses:
                //     1. Remove the item whose value matches requestResponse from cache.
                remove_items_from_cache(request_responses);
            }
            // 6. Else if operation’s type matches "put", then:
            else if (operation->type == CacheBatchOperation::Type::Put) {
                // 1. If operation’s response is null, throw a TypeError.
                if (!operation->response)
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Batch operations of type 'put' must have a response"sv };

                // 2. Let r be operation’s request’s associated request.
                auto request = operation->request;

                // 3. If r’s url’s scheme is not one of "http" and "https", throw a TypeError.
                if (!request->url().scheme().is_one_of("http"sv, "https"sv))
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request must be a GET request with an HTTP(S) URL"sv };

                // 4. If r’s method is not `GET`, throw a TypeError.
                if (request->method() != "GET"sv)
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request must be a GET request with an HTTP(S) URL"sv };

                // 5. If operation’s options is not null, throw a TypeError.
                // FIXME: Spec issue: No other part of the spec indicates that options may be created as null.

                // 6. Set requestResponses to the result of running Query Cache with operation’s request.
                request_responses = query_cache(operation->request, {}, {}, CloneCache::No);

                // 7. For each requestResponse of requestResponses:
                //     1. Remove the item whose value matches requestResponse from cache.
                remove_items_from_cache(request_responses);

                // 8. Append operation’s request/operation’s response to cache.
                cache->elements().append(realm.heap().allocate<RequestResponse>(operation->request, *operation->response));

                // FIXME: 9. If the cache write operation in the previous two steps failed due to exceeding the granted quota
                //           limit, throw a QuotaExceededError.

                // 10. Append operation’s request/operation’s response to addedItems.
                added_items->elements().append(realm.heap().allocate<RequestResponse>(operation->request, *operation->response));
            }

            // 7. Append operation’s request/operation’s response to resultList.
        }

        // 3. Return resultList.
        return removed_any_items;
    }();

    // 5. And then, if an exception was thrown, then:
    if (result.is_error()) {
        // 1. Remove all the items from the relevant request response list.
        // 2. For each requestResponse of backupCache:
        //     1. Append requestResponse to the relevant request response list.
        m_request_response_list = backup_cache;

        // 3. Throw the exception.
        return result.release_error();

        // Note: When an exception is thrown, the implementation does undo (roll back) any changes made to the cache
        //       storage during the batch operation job.
    }

    return result.release_value();
}

void CacheBatchOperation::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(request);
    visitor.visit(response);
}

}
