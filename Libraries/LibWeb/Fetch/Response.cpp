/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/ResponsePrototype.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Enums.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Infra/JSON.h>

namespace Web::Fetch {

GC_DEFINE_ALLOCATOR(Response);

Response::Response(JS::Realm& realm, GC::Ref<Infrastructure::Response> response)
    : PlatformObject(realm)
    , m_response(response)
{
}

Response::~Response() = default;

void Response::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Response);
}

void Response::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_response);
    visitor.visit(m_headers);
}

// https://fetch.spec.whatwg.org/#concept-body-mime-type
// https://fetch.spec.whatwg.org/#ref-for-concept-header-extract-mime-type%E2%91%A7
Optional<MimeSniff::MimeType> Response::mime_type_impl() const
{
    // Objects including the Body interface mixin need to define an associated MIME type algorithm which takes no arguments and returns failure or a MIME type.
    // A Response object’s MIME type is to return the result of extracting a MIME type from its response’s header list.
    return m_response->header_list()->extract_mime_type();
}

// https://fetch.spec.whatwg.org/#concept-body-body
// https://fetch.spec.whatwg.org/#ref-for-concept-body-body%E2%91%A8
GC::Ptr<Infrastructure::Body const> Response::body_impl() const
{
    // Objects including the Body interface mixin have an associated body (null or a body).
    // A Response object’s body is its response’s body.
    return m_response->body() ? m_response->body() : nullptr;
}

// https://fetch.spec.whatwg.org/#concept-body-body
// https://fetch.spec.whatwg.org/#ref-for-concept-body-body%E2%91%A8
GC::Ptr<Infrastructure::Body> Response::body_impl()
{
    // Objects including the Body interface mixin have an associated body (null or a body).
    // A Response object’s body is its response’s body.
    return m_response->body() ? m_response->body() : nullptr;
}

// https://fetch.spec.whatwg.org/#response-create
GC::Ref<Response> Response::create(JS::Realm& realm, GC::Ref<Infrastructure::Response> response, Headers::Guard guard)
{
    // 1. Let responseObject be a new Response object with realm.
    // 2. Set responseObject’s response to response.
    auto response_object = realm.create<Response>(realm, response);

    // 3. Set responseObject’s headers to a new Headers object with realm, whose headers list is response’s headers list and guard is guard.
    response_object->m_headers = realm.create<Headers>(realm, response->header_list());
    response_object->m_headers->set_guard(guard);

    // 4. Return responseObject.
    return response_object;
}

// https://httpwg.org/specs/rfc9112.html#status.line
static bool is_valid_status_text(String const& status_text)
{
    // A status text is valid if it is either the empty string or matches the reason-phrase token production.
    // reason-phrase  = 1*( HTAB / SP / VCHAR / obs-text )
    // VCHAR          = %x21-7E
    // obs-text       = %x80-FF
    return all_of(status_text.code_points(), [](auto c) {
        return c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
    });
}

// https://fetch.spec.whatwg.org/#initialize-a-response
WebIDL::ExceptionOr<void> Response::initialize_response(ResponseInit const& init, Optional<Infrastructure::BodyWithType> const& body)
{
    // 1. If init["status"] is not in the range 200 to 599, inclusive, then throw a RangeError.
    if (init.status < 200 || init.status > 599)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Status must be in range 200-599"sv };

    // 2. If init["statusText"] is not the empty string and does not match the reason-phrase token production, then throw a TypeError.
    if (!is_valid_status_text(init.status_text))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid statusText: does not match the reason-phrase token production"sv };

    // 3. Set response’s response’s status to init["status"].
    m_response->set_status(init.status);

    // 4. Set response’s response’s status message to init["statusText"].
    m_response->set_status_message(MUST(ByteBuffer::copy(init.status_text.bytes())));

    // 5. If init["headers"] exists, then fill response’s headers with init["headers"].
    if (init.headers.has_value())
        TRY(m_headers->fill(*init.headers));

    // 6. If body was given, then:
    if (body.has_value()) {
        // 1. If response’s status is a null body status, then throw a TypeError.
        if (Infrastructure::is_null_body_status(m_response->status()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Response with null body status cannot have a body"sv };

        // 2. Set response’s body to body’s body.
        m_response->set_body(body->body);

        // 3. If body’s type is non-null and response’s header list does not contain `Content-Type`, then append (`Content-Type`, body’s type) to response’s header list.
        if (body->type.has_value() && !m_response->header_list()->contains("Content-Type"sv.bytes())) {
            auto header = Infrastructure::Header {
                .name = MUST(ByteBuffer::copy("Content-Type"sv.bytes())),
                .value = MUST(ByteBuffer::copy(body->type->span())),
            };
            m_response->header_list()->append(move(header));
        }
    }

    return {};
}

// https://fetch.spec.whatwg.org/#dom-response
WebIDL::ExceptionOr<GC::Ref<Response>> Response::construct_impl(JS::Realm& realm, Optional<BodyInit> const& body, ResponseInit const& init)
{
    auto& vm = realm.vm();

    // Referred to as 'this' in the spec.
    auto response_object = realm.create<Response>(realm, Infrastructure::Response::create(vm));

    // 1. Set this’s response to a new response.
    // NOTE: This is done at the beginning as the 'this' value Response object
    //       cannot exist with a null Infrastructure::Response.

    // 2. Set this’s headers to a new Headers object with this’s relevant Realm, whose header list is this’s response’s header list and guard is "response".
    response_object->m_headers = realm.create<Headers>(realm, response_object->response()->header_list());
    response_object->m_headers->set_guard(Headers::Guard::Response);

    // 3. Let bodyWithType be null.
    Optional<Infrastructure::BodyWithType> body_with_type;

    // 4. If body is non-null, then set bodyWithType to the result of extracting body.
    if (body.has_value())
        body_with_type = TRY(extract_body(realm, *body));

    // 5. Perform initialize a response given this, init, and bodyWithType.
    TRY(response_object->initialize_response(init, body_with_type));

    return response_object;
}

// https://fetch.spec.whatwg.org/#dom-response-error
GC::Ref<Response> Response::error(JS::VM& vm)
{
    // The static error() method steps are to return the result of creating a Response object, given a new network error, "immutable", and this’s relevant Realm.
    // FIXME: How can we reliably get 'this', i.e. the object the function was called on, in IDL-defined functions?
    return Response::create(*vm.current_realm(), Infrastructure::Response::network_error(vm, "Response created via `Response.error()`"_string), Headers::Guard::Immutable);
}

// https://fetch.spec.whatwg.org/#dom-response-redirect
WebIDL::ExceptionOr<GC::Ref<Response>> Response::redirect(JS::VM& vm, String const& url, u16 status)
{
    auto& realm = *vm.current_realm();

    // 1. Let parsedURL be the result of parsing url with current settings object’s API base URL.
    auto api_base_url = HTML::current_principal_settings_object().api_base_url();
    auto parsed_url = DOMURL::parse(url, api_base_url);

    // 2. If parsedURL is failure, then throw a TypeError.
    if (!parsed_url.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Redirect URL is not valid"sv };

    // 3. If status is not a redirect status, then throw a RangeError.
    if (!Infrastructure::is_redirect_status(status))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Status must be one of 301, 302, 303, 307, or 308"sv };

    // 4. Let responseObject be the result of creating a Response object, given a new response, "immutable", and this’s relevant Realm.
    // FIXME: How can we reliably get 'this', i.e. the object the function was called on, in IDL-defined functions?
    auto response_object = Response::create(realm, Infrastructure::Response::create(vm), Headers::Guard::Immutable);

    // 5. Set responseObject’s response’s status to status.
    response_object->response()->set_status(status);

    // 6. Let value be parsedURL, serialized and isomorphic encoded.
    auto value = parsed_url->serialize();

    // 7. Append (`Location`, value) to responseObject’s response’s header list.
    auto header = Infrastructure::Header::from_string_pair("Location"sv, value);
    response_object->response()->header_list()->append(move(header));

    // 8. Return responseObject.
    return response_object;
}

// https://fetch.spec.whatwg.org/#dom-response-json
WebIDL::ExceptionOr<GC::Ref<Response>> Response::json(JS::VM& vm, JS::Value data, ResponseInit const& init)
{
    auto& realm = *vm.current_realm();

    // 1. Let bytes the result of running serialize a JavaScript value to JSON bytes on data.
    auto bytes = TRY(Infra::serialize_javascript_value_to_json_bytes(vm, data));

    // 2. Let body be the result of extracting bytes.
    auto [body, _] = TRY(extract_body(realm, { bytes.bytes() }));

    // 3. Let responseObject be the result of creating a Response object, given a new response, "response", and this’s relevant Realm.
    // FIXME: How can we reliably get 'this', i.e. the object the function was called on, in IDL-defined functions?
    auto response_object = Response::create(realm, Infrastructure::Response::create(vm), Headers::Guard::Response);

    // 4. Perform initialize a response given responseObject, init, and (body, "application/json").
    auto body_with_type = Infrastructure::BodyWithType {
        .body = body,
        .type = MUST(ByteBuffer::copy("application/json"sv.bytes()))
    };
    TRY(response_object->initialize_response(init, move(body_with_type)));

    // 5. Return responseObject.
    return response_object;
}

// https://fetch.spec.whatwg.org/#dom-response-type
Bindings::ResponseType Response::type() const
{
    // The type getter steps are to return this’s response’s type.
    return to_bindings_enum(m_response->type());
}

// https://fetch.spec.whatwg.org/#dom-response-url
String Response::url() const
{
    // The url getter steps are to return the empty string if this’s response’s URL is null; otherwise this’s response’s URL, serialized with exclude fragment set to true.
    return !m_response->url().has_value()
        ? String {}
        : m_response->url()->serialize(URL::ExcludeFragment::Yes);
}

// https://fetch.spec.whatwg.org/#dom-response-redirected
bool Response::redirected() const
{
    // The redirected getter steps are to return true if this’s response’s URL list has more than one item; otherwise false.
    return m_response->url_list().size() > 1;
}

// https://fetch.spec.whatwg.org/#dom-response-status
u16 Response::status() const
{
    // The status getter steps are to return this’s response’s status.
    return m_response->status();
}

// https://fetch.spec.whatwg.org/#dom-response-ok
bool Response::ok() const
{
    // The ok getter steps are to return true if this’s response’s status is an ok status; otherwise false.
    return Infrastructure::is_ok_status(m_response->status());
}

// https://fetch.spec.whatwg.org/#dom-response-statustext
String Response::status_text() const
{
    // The statusText getter steps are to return this’s response’s status message.
    return MUST(String::from_utf8(m_response->status_message()));
}

// https://fetch.spec.whatwg.org/#dom-response-headers
GC::Ref<Headers> Response::headers() const
{
    // The headers getter steps are to return this’s headers.
    return *m_headers;
}

// https://fetch.spec.whatwg.org/#dom-response-clone
WebIDL::ExceptionOr<GC::Ref<Response>> Response::clone() const
{
    auto& realm = this->realm();

    // 1. If this is unusable, then throw a TypeError.
    if (is_unusable())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Response is unusable"sv };

    // 2. Let clonedResponse be the result of cloning this’s response.
    auto cloned_response = m_response->clone(realm);

    // 3. Return the result of creating a Response object, given clonedResponse, this’s headers’s guard, and this’s relevant Realm.
    return Response::create(HTML::relevant_realm(*this), cloned_response, m_headers->guard());
}

}
