/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/RequestPrototype.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Enums.h>
#include <LibWeb/Fetch/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Methods.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::Fetch {

GC_DEFINE_ALLOCATOR(Request);

Request::Request(JS::Realm& realm, GC::Ref<Infrastructure::Request> request)
    : PlatformObject(realm)
    , m_request(request)
{
}

Request::~Request() = default;

void Request::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Request);
}

void Request::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request);
    visitor.visit(m_headers);
    visitor.visit(m_signal);
}

// https://fetch.spec.whatwg.org/#concept-body-mime-type
// https://fetch.spec.whatwg.org/#ref-for-concept-body-mime-type%E2%91%A0
Optional<MimeSniff::MimeType> Request::mime_type_impl() const
{
    // Objects including the Body interface mixin need to define an associated MIME type algorithm which takes no arguments and returns failure or a MIME type.
    // A Request object’s MIME type is to return the result of extracting a MIME type from its request’s header list.
    return m_request->header_list()->extract_mime_type();
}

// https://fetch.spec.whatwg.org/#concept-body-body
// https://fetch.spec.whatwg.org/#ref-for-concept-body-body%E2%91%A7
GC::Ptr<Infrastructure::Body const> Request::body_impl() const
{
    // Objects including the Body interface mixin have an associated body (null or a body).
    // A Request object’s body is its request’s body.
    return m_request->body().visit(
        [](GC::Ref<Infrastructure::Body> const& b) -> GC::Ptr<Infrastructure::Body const> { return b; },
        [](Empty) -> GC::Ptr<Infrastructure::Body const> { return nullptr; },
        // A byte sequence will be safely extracted into a body early on in fetch.
        [](ByteBuffer const&) -> GC::Ptr<Infrastructure::Body const> { VERIFY_NOT_REACHED(); });
}

// https://fetch.spec.whatwg.org/#concept-body-body
// https://fetch.spec.whatwg.org/#ref-for-concept-body-body%E2%91%A7
GC::Ptr<Infrastructure::Body> Request::body_impl()
{
    // Objects including the Body interface mixin have an associated body (null or a body).
    // A Request object’s body is its request’s body.
    return m_request->body().visit(
        [](GC::Ref<Infrastructure::Body>& b) -> GC::Ptr<Infrastructure::Body> { return b; },
        [](Empty) -> GC::Ptr<Infrastructure::Body> { return {}; },
        // A byte sequence will be safely extracted into a body early on in fetch.
        [](ByteBuffer&) -> GC::Ptr<Infrastructure::Body> { VERIFY_NOT_REACHED(); });
}

// https://fetch.spec.whatwg.org/#request-create
GC::Ref<Request> Request::create(JS::Realm& realm, GC::Ref<Infrastructure::Request> request, Headers::Guard guard, GC::Ref<DOM::AbortSignal> signal)
{
    // 1. Let requestObject be a new Request object with realm.
    // 2. Set requestObject’s request to request.
    auto request_object = realm.create<Request>(realm, request);

    // 3. Set requestObject’s headers to a new Headers object with realm, whose headers list is request’s headers list and guard is guard.
    request_object->m_headers = realm.create<Headers>(realm, request->header_list());
    request_object->m_headers->set_guard(guard);

    // 4. Set requestObject’s signal to signal.
    request_object->m_signal = signal;

    // 5. Return requestObject.
    return request_object;
}

// https://fetch.spec.whatwg.org/#dom-request
WebIDL::ExceptionOr<GC::Ref<Request>> Request::construct_impl(JS::Realm& realm, RequestInfo const& input, RequestInit const& init)
{
    auto& vm = realm.vm();

    // Referred to as 'this' in the spec.
    auto request_object = realm.create<Request>(realm, Infrastructure::Request::create(vm));

    // 1. Let request be null.
    GC::Ptr<Infrastructure::Request> input_request;

    // 2. Let fallbackMode be null.
    Optional<Infrastructure::Request::Mode> fallback_mode;

    // 3. Let baseURL be this’s relevant settings object’s API base URL.
    auto base_url = HTML::relevant_settings_object(*request_object).api_base_url();

    // 4. Let signal be null.
    DOM::AbortSignal* input_signal = nullptr;

    // 5. If input is a string, then:
    if (input.has<String>()) {
        // 1. Let parsedURL be the result of parsing input with baseURL.
        auto parsed_url = DOMURL::parse(input.get<String>(), base_url);

        // 2. If parsedURL is failure, then throw a TypeError.
        if (!parsed_url.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Input URL is not valid"sv };

        // 3. If parsedURL includes credentials, then throw a TypeError.
        if (parsed_url->includes_credentials())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Input URL must not include credentials"sv };

        // 4. Set request to a new request whose URL is parsedURL.
        input_request = Infrastructure::Request::create(vm);
        input_request->set_url(parsed_url.release_value());

        // 5. Set fallbackMode to "cors".
        fallback_mode = Infrastructure::Request::Mode::CORS;
    }
    // 6. Otherwise:
    else {
        // 1. Assert: input is a Request object.
        VERIFY(input.has<GC::Root<Request>>());

        // 2. Set request to input’s request.
        input_request = input.get<GC::Root<Request>>()->request();

        // 3. Set signal to input’s signal.
        input_signal = input.get<GC::Root<Request>>()->signal();
    }

    // 7. Let origin be this’s relevant settings object’s origin.
    auto const& origin = HTML::relevant_settings_object(*request_object).origin();

    // 8. Let window be "client".
    auto window = Infrastructure::Request::WindowType { Infrastructure::Request::Window::Client };

    // 9. If request’s window is an environment settings object and its origin is same origin with origin, then set window to request’s window.
    if (input_request->window().has<GC::Ptr<HTML::EnvironmentSettingsObject>>()) {
        auto eso = input_request->window().get<GC::Ptr<HTML::EnvironmentSettingsObject>>();
        if (eso->origin().is_same_origin(origin))
            window = input_request->window();
    }

    // 10. If init["window"] exists and is non-null, then throw a TypeError.
    if (init.window.has_value() && !init.window->is_null())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The 'window' property must be omitted or null"sv };

    // 11. If init["window"] exists, then set window to "no-window".
    if (init.window.has_value())
        window = Infrastructure::Request::Window::NoWindow;

    // 12. Set request to a new request with the following properties:
    // NOTE: This is done at the beginning as the 'this' value Request object
    //       cannot exist with a null Infrastructure::Request.
    auto request = request_object->request();

    // URL
    //     request’s URL.
    request->set_url(input_request->url());

    // method
    //     request’s method.
    request->set_method(MUST(ByteBuffer::copy(input_request->method())));

    // header list
    //     A copy of request’s header list.
    auto header_list_copy = Infrastructure::HeaderList::create(vm);
    for (auto& header : *input_request->header_list())
        header_list_copy->append(header);
    request->set_header_list(header_list_copy);

    // unsafe-request flag
    //     Set.
    request->set_unsafe_request(true);

    // client
    //     This’s relevant settings object.
    request->set_client(&HTML::relevant_settings_object(*request_object));

    // window
    //     window.
    request->set_window(window);

    // priority
    //     request’s priority.
    request->set_priority(input_request->priority());

    // origin
    //     request’s origin. The propagation of the origin is only significant for navigation requests being handled by a service worker. In this scenario a request can have an origin that is different from the current client.
    request->set_origin(input_request->origin());

    // referrer
    //     request’s referrer.
    request->set_referrer(input_request->referrer());

    // referrer policy
    //     request’s referrer policy.
    request->set_referrer_policy(input_request->referrer_policy());

    // mode
    //     request’s mode.
    request->set_mode(input_request->mode());

    // credentials mode
    //     request’s credentials mode.
    request->set_credentials_mode(input_request->credentials_mode());

    // cache mode
    //     request’s cache mode.
    request->set_cache_mode(input_request->cache_mode());

    // redirect mode
    //     request’s redirect mode.
    request->set_redirect_mode(input_request->redirect_mode());

    // integrity metadata
    //     request’s integrity metadata.
    request->set_integrity_metadata(input_request->integrity_metadata());

    // keepalive
    //     request’s keepalive.
    request->set_keepalive(input_request->keepalive());

    // reload-navigation flag
    //     request’s reload-navigation flag.
    request->set_reload_navigation(input_request->reload_navigation());

    // history-navigation flag
    //     request’s history-navigation flag.
    request->set_history_navigation(input_request->history_navigation());

    // URL list
    //     A clone of request’s URL list.
    request->set_url_list(input_request->url_list());

    // initiator type
    //     "fetch".
    request->set_initiator_type(Infrastructure::Request::InitiatorType::Fetch);

    // 13. If init is not empty, then:
    if (!init.is_empty()) {
        // 1. If request’s mode is "navigate", then set it to "same-origin".
        if (request->mode() == Infrastructure::Request::Mode::Navigate)
            request->set_mode(Infrastructure::Request::Mode::SameOrigin);

        // 2. Unset request’s reload-navigation flag.
        request->set_reload_navigation(false);

        // 3. Unset request’s history-navigation flag.
        request->set_history_navigation(false);

        // 4. Set request’s origin to "client".
        request->set_origin(Infrastructure::Request::Origin::Client);

        // 5. Set request’s referrer to "client".
        request->set_referrer(Infrastructure::Request::Referrer::Client);

        // 6. Set request’s referrer policy to the empty string.
        request->set_referrer_policy({});

        // 7. Set request’s URL to request’s current URL.
        request->set_url(request->current_url());

        // 8. Set request’s URL list to « request’s URL ».
        // NOTE: This is done implicitly by assigning the initial URL above.
    }

    // 14. If init["referrer"] exists, then:
    if (init.referrer.has_value()) {
        // 1. Let referrer be init["referrer"].
        auto const& referrer = *init.referrer;

        // 2. If referrer is the empty string, then set request’s referrer to "no-referrer".
        if (referrer.is_empty()) {
            request->set_referrer(Infrastructure::Request::Referrer::NoReferrer);
        }
        // 3. Otherwise:
        else {
            // 1. Let parsedReferrer be the result of parsing referrer with baseURL.
            auto parsed_referrer = DOMURL::parse(referrer, base_url);

            // 2. If parsedReferrer is failure, then throw a TypeError.
            if (!parsed_referrer.has_value())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Referrer must be a valid URL"sv };

            // 3. If one of the following is true
            // - parsedReferrer’s scheme is "about" and path is the string "client"
            // - parsedReferrer’s origin is not same origin with origin
            // then set request’s referrer to "client".
            auto parsed_referrer_origin = parsed_referrer->origin();
            if ((parsed_referrer->scheme() == "about"sv && parsed_referrer->paths().size() == 1 && parsed_referrer->paths()[0] == "client"sv)
                || !parsed_referrer_origin.is_same_origin(origin)) {
                request->set_referrer(Infrastructure::Request::Referrer::Client);
            }
            // 4. Otherwise, set request’s referrer to parsedReferrer.
            else {
                request->set_referrer(parsed_referrer.release_value());
            }
        }
    }

    // 15. If init["referrerPolicy"] exists, then set request’s referrer policy to it.
    if (init.referrer_policy.has_value())
        request->set_referrer_policy(from_bindings_enum(*init.referrer_policy));

    // 16. Let mode be init["mode"] if it exists, and fallbackMode otherwise.
    auto mode = init.mode.has_value()
        ? from_bindings_enum(*init.mode)
        : fallback_mode;

    // 17. If mode is "navigate", then throw a TypeError.
    if (mode == Infrastructure::Request::Mode::Navigate)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Mode must not be 'navigate"sv };

    // 18. If mode is non-null, set request’s mode to mode.
    if (mode.has_value())
        request->set_mode(*mode);

    // 19. If init["credentials"] exists, then set request’s credentials mode to it.
    if (init.credentials.has_value())
        request->set_credentials_mode(from_bindings_enum(*init.credentials));

    // 20. If init["cache"] exists, then set request’s cache mode to it.
    if (init.cache.has_value())
        request->set_cache_mode(from_bindings_enum(*init.cache));

    // 21. If request’s cache mode is "only-if-cached" and request’s mode is not "same-origin", then throw a TypeError.
    if (request->cache_mode() == Infrastructure::Request::CacheMode::OnlyIfCached && request->mode() != Infrastructure::Request::Mode::SameOrigin)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Mode must be 'same-origin' when cache mode is 'only-if-cached'"sv };

    // 22. If init["redirect"] exists, then set request’s redirect mode to it.
    if (init.redirect.has_value())
        request->set_redirect_mode(from_bindings_enum(*init.redirect));

    // 23. If init["integrity"] exists, then set request’s integrity metadata to it.
    if (init.integrity.has_value())
        request->set_integrity_metadata(*init.integrity);

    // 24. If init["keepalive"] exists, then set request’s keepalive to it.
    if (init.keepalive.has_value())
        request->set_keepalive(*init.keepalive);

    // 25. If init["method"] exists, then:
    if (init.method.has_value()) {
        // 1. Let method be init["method"].
        auto method = *init.method;

        // 2. If method is not a method or method is a forbidden method, then throw a TypeError.
        if (!Infrastructure::is_method(method.bytes()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Method has invalid value"sv };
        if (Infrastructure::is_forbidden_method(method.bytes()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Method must not be one of CONNECT, TRACE, or TRACK"sv };

        // 3. Normalize method.
        method = MUST(String::from_utf8(Infrastructure::normalize_method(method.bytes())));

        // 4. Set request’s method to method.
        request->set_method(MUST(ByteBuffer::copy(method.bytes())));
    }

    // 26. If init["signal"] exists, then set signal to it.
    if (init.signal.has_value())
        input_signal = *init.signal;

    // 27. If init["priority"] exists, then:
    if (init.priority.has_value())
        request->set_priority(from_bindings_enum(*init.priority));

    // 28. Set this’s request to request.
    // NOTE: This is done at the beginning as the 'this' value Request object
    //       cannot exist with a null Infrastructure::Request.

    // 29. Let signals be « signal » if signal is non-null; otherwise « ».
    auto& this_relevant_realm = HTML::relevant_realm(*request_object);
    Vector<GC::Root<DOM::AbortSignal>> signals;
    if (input_signal != nullptr)
        signals.append(*input_signal);

    // 30. Set this’s signal to the result of creating a dependent abort signal from signals, using AbortSignal and this’s relevant realm.
    request_object->m_signal = TRY(DOM::AbortSignal::create_dependent_abort_signal(this_relevant_realm, signals));

    // 31. Set this’s headers to a new Headers object with this’s relevant Realm, whose header list is request’s header list and guard is "request".
    request_object->m_headers = realm.create<Headers>(realm, request->header_list());
    request_object->m_headers->set_guard(Headers::Guard::Request);

    // 32. If this’s request’s mode is "no-cors", then:
    if (request_object->request()->mode() == Infrastructure::Request::Mode::NoCORS) {
        // 1. If this’s request’s method is not a CORS-safelisted method, then throw a TypeError.
        if (!Infrastructure::is_cors_safelisted_method(request_object->request()->method()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Method must be one of GET, HEAD, or POST"sv };

        // 2. Set this’s headers’s guard to "request-no-cors".
        request_object->headers()->set_guard(Headers::Guard::RequestNoCORS);
    }

    // 33. If init is not empty, then:
    if (!init.is_empty()) {
        // 1. Let headers be a copy of this’s headers and its associated header list.
        auto headers = Variant<HeadersInit, GC::Ref<Infrastructure::HeaderList>> { request_object->headers()->header_list() };

        // 2. If init["headers"] exists, then set headers to init["headers"].
        if (init.headers.has_value())
            headers = *init.headers;

        // 3. Empty this’s headers’s header list.
        request_object->headers()->header_list()->clear();

        // 4. If headers is a Headers object, then for each header of its header list, append header to this’s headers.
        if (auto* header_list = headers.get_pointer<GC::Ref<Infrastructure::HeaderList>>()) {
            for (auto& header : *header_list->ptr())
                TRY(request_object->headers()->append(Infrastructure::Header::from_string_pair(header.name, header.value)));
        }
        // 5. Otherwise, fill this’s headers with headers.
        else {
            TRY(request_object->headers()->fill(headers.get<HeadersInit>()));
        }
    }

    // 34. Let inputBody be input’s request’s body if input is a Request object; otherwise null.
    Optional<Infrastructure::Request::BodyType const&> input_body;
    if (input.has<GC::Root<Request>>())
        input_body = input.get<GC::Root<Request>>()->request()->body();

    // 35. If either init["body"] exists and is non-null or inputBody is non-null, and request’s method is `GET` or `HEAD`, then throw a TypeError.
    if (((init.body.has_value() && (*init.body).has_value()) || (input_body.has_value() && !input_body.value().has<Empty>())) && StringView { request->method() }.is_one_of("GET"sv, "HEAD"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Method must not be GET or HEAD when body is provided"sv };

    // 36. Let initBody be null.
    Optional<Infrastructure::Request::BodyType> init_body;

    // 37. If init["body"] exists and is non-null, then:
    if (init.body.has_value() && (*init.body).has_value()) {
        // 1. Let bodyWithType be the result of extracting init["body"], with keepalive set to request’s keepalive.
        auto body_with_type = TRY(extract_body(realm, (*init.body).value(), request->keepalive()));

        // 2. Set initBody to bodyWithType’s body.
        init_body = body_with_type.body;

        // 3. Let type be bodyWithType’s type.
        auto const& type = body_with_type.type;

        // 4. If type is non-null and this’s headers’s header list does not contain `Content-Type`, then append (`Content-Type`, type) to this’s headers.
        if (type.has_value() && !request_object->headers()->header_list()->contains("Content-Type"sv.bytes()))
            TRY(request_object->headers()->append(Infrastructure::Header::from_string_pair("Content-Type"sv, type->span())));
    }

    // 38. Let inputOrInitBody be initBody if it is non-null; otherwise inputBody.
    auto input_or_init_body = init_body.value_or<Optional<Infrastructure::Request::BodyType const&>>(input_body);

    // 39. If inputOrInitBody is non-null and inputOrInitBody’s source is null, then:
    // FIXME: The spec doesn't check if inputOrInitBody is a body before accessing source.
    if (input_or_init_body.has_value() && input_or_init_body->has<GC::Ref<Infrastructure::Body>>() && input_or_init_body->get<GC::Ref<Infrastructure::Body>>()->source().has<Empty>()) {
        // 1. If initBody is non-null and init["duplex"] does not exist, then throw a TypeError.
        if (init_body.has_value() && !init.duplex.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Body without source requires 'duplex' value to be set"sv };

        // 2. If this’s request’s mode is neither "same-origin" nor "cors", then throw a TypeError.
        if (request_object->request()->mode() != Infrastructure::Request::Mode::SameOrigin && request_object->request()->mode() != Infrastructure::Request::Mode::CORS)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request mode must be 'same-origin' or 'cors'"sv };

        // 3. Set this’s request’s use-CORS-preflight flag.
        request_object->request()->set_use_cors_preflight(true);
    }

    // 40. Let finalBody be inputOrInitBody.
    auto const& final_body = input_or_init_body;

    // 41. If initBody is null and inputBody is non-null, then:
    if (!init_body.has_value() && input_body.has_value()) {
        // 2. If input is unusable, then throw a TypeError.
        if (input.has<GC::Root<Request>>() && input.get<GC::Root<Request>>()->is_unusable())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request is unusable"sv };

        // FIXME: 2. Set finalBody to the result of creating a proxy for inputBody.
    }

    // 42. Set this’s request’s body to finalBody.
    if (final_body.has_value())
        request_object->request()->set_body(*final_body);

    return GC::Ref { *request_object };
}

// https://fetch.spec.whatwg.org/#dom-request-method
String Request::method() const
{
    // The method getter steps are to return this’s request’s method.
    return MUST(String::from_utf8(m_request->method()));
}

// https://fetch.spec.whatwg.org/#dom-request-url
String Request::url() const
{
    // The url getter steps are to return this’s request’s URL, serialized.
    return m_request->url().serialize();
}

// https://fetch.spec.whatwg.org/#dom-request-headers
GC::Ref<Headers> Request::headers() const
{
    // The headers getter steps are to return this’s headers.
    return *m_headers;
}

// https://fetch.spec.whatwg.org/#dom-request-destination
Bindings::RequestDestination Request::destination() const
{
    // The destination getter are to return this’s request’s destination.
    return to_bindings_enum(m_request->destination());
}

// https://fetch.spec.whatwg.org/#dom-request-referrer
String Request::referrer() const
{
    return m_request->referrer().visit(
        [&](Infrastructure::Request::Referrer const& referrer) {
            switch (referrer) {
            // 1. If this’s request’s referrer is "no-referrer", then return the empty string.
            case Infrastructure::Request::Referrer::NoReferrer:
                return String {};
            // 2. If this’s request’s referrer is "client", then return "about:client".
            case Infrastructure::Request::Referrer::Client:
                return "about:client"_string;
            default:
                VERIFY_NOT_REACHED();
            }
        },
        [&](URL::URL const& url) {
            // 3. Return this’s request’s referrer, serialized.
            return url.serialize();
        });
}

// https://fetch.spec.whatwg.org/#dom-request-referrerpolicy
Bindings::ReferrerPolicy Request::referrer_policy() const
{
    // The referrerPolicy getter steps are to return this’s request’s referrer policy.
    return to_bindings_enum(m_request->referrer_policy());
}

// https://fetch.spec.whatwg.org/#dom-request-mode
Bindings::RequestMode Request::mode() const
{
    // The mode getter steps are to return this’s request’s mode.
    return to_bindings_enum(m_request->mode());
}

// https://fetch.spec.whatwg.org/#dom-request-credentials
Bindings::RequestCredentials Request::credentials() const
{
    // The credentials getter steps are to return this’s request’s credentials mode.
    return to_bindings_enum(m_request->credentials_mode());
}

// https://fetch.spec.whatwg.org/#dom-request-cache
Bindings::RequestCache Request::cache() const
{
    // The cache getter steps are to return this’s request’s cache mode.
    return to_bindings_enum(m_request->cache_mode());
}

// https://fetch.spec.whatwg.org/#dom-request-redirect
Bindings::RequestRedirect Request::redirect() const
{
    // The redirect getter steps are to return this’s request’s redirect mode.
    return to_bindings_enum(m_request->redirect_mode());
}

// https://fetch.spec.whatwg.org/#dom-request-integrity
String Request::integrity() const
{
    // The integrity getter steps are to return this’s request’s integrity metadata.
    return m_request->integrity_metadata();
}

// https://fetch.spec.whatwg.org/#dom-request-keepalive
bool Request::keepalive() const
{
    // The keepalive getter steps are to return this’s request’s keepalive.
    return m_request->keepalive();
}

// https://fetch.spec.whatwg.org/#dom-request-isreloadnavigation
bool Request::is_reload_navigation() const
{
    // The isReloadNavigation getter steps are to return true if this’s request’s reload-navigation flag is set; otherwise false.
    return m_request->reload_navigation();
}

// https://fetch.spec.whatwg.org/#dom-request-ishistorynavigation
bool Request::is_history_navigation() const
{
    // The isHistoryNavigation getter steps are to return true if this’s request’s history-navigation flag is set; otherwise false.
    return m_request->history_navigation();
}

// https://fetch.spec.whatwg.org/#dom-request-signal
GC::Ref<DOM::AbortSignal> Request::signal() const
{
    // The signal getter steps are to return this’s signal.
    return *m_signal;
}

// https://fetch.spec.whatwg.org/#dom-request-duplex
Bindings::RequestDuplex Request::duplex() const
{
    // The duplex getter steps are to return "half".
    return Bindings::RequestDuplex::Half;
}

// https://fetch.spec.whatwg.org/#dom-request-clone
WebIDL::ExceptionOr<GC::Ref<Request>> Request::clone() const
{
    auto& realm = this->realm();

    // 1. If this is unusable, then throw a TypeError.
    if (is_unusable())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request is unusable"sv };

    // 2. Let clonedRequest be the result of cloning this’s request.
    auto cloned_request = m_request->clone(realm);

    // 3. Assert: this’s signal is non-null.
    VERIFY(m_signal);

    // 4. Let clonedSignal be the result of creating a dependent abort signal from « this’s signal », using AbortSignal and this’s relevant realm.
    auto& relevant_realm = HTML::relevant_realm(*this);
    auto cloned_signal = TRY(DOM::AbortSignal::create_dependent_abort_signal(relevant_realm, { m_signal }));

    // 5. Let clonedRequestObject be the result of creating a Request object, given clonedRequest, this’s headers’s guard, clonedSignal and this’s relevant realm.
    auto cloned_request_object = Request::create(relevant_realm, cloned_request, m_headers->guard(), cloned_signal);

    // 6. Return clonedRequestObject.
    return cloned_request_object;
}

}
