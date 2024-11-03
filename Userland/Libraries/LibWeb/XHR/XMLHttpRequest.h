/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XHR/XMLHttpRequestEventTarget.h>

namespace Web::XHR {

// https://fetch.spec.whatwg.org/#typedefdef-xmlhttprequestbodyinit
using DocumentOrXMLHttpRequestBodyInit = Variant<JS::Handle<Web::DOM::Document>, JS::Handle<Web::FileAPI::Blob>, JS::Handle<WebIDL::BufferSource>, JS::Handle<XHR::FormData>, JS::Handle<Web::DOMURL::URLSearchParams>, AK::String>;

class XMLHttpRequest final : public XMLHttpRequestEventTarget {
    WEB_PLATFORM_OBJECT(XMLHttpRequest, XMLHttpRequestEventTarget);
    JS_DECLARE_ALLOCATOR(XMLHttpRequest);

public:
    enum class State : u16 {
        Unsent = 0,
        Opened = 1,
        HeadersReceived = 2,
        Loading = 3,
        Done = 4,
    };

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<XMLHttpRequest>> construct_impl(JS::Realm&);

    virtual ~XMLHttpRequest() override;

    State ready_state() const { return m_state; }
    Fetch::Infrastructure::Status status() const;
    WebIDL::ExceptionOr<String> status_text() const;
    WebIDL::ExceptionOr<String> response_text() const;
    WebIDL::ExceptionOr<JS::GCPtr<DOM::Document>> response_xml();
    WebIDL::ExceptionOr<JS::Value> response();
    Bindings::XMLHttpRequestResponseType response_type() const { return m_response_type; }
    String response_url();

    WebIDL::ExceptionOr<void> open(String const& method, String const& url);
    WebIDL::ExceptionOr<void> open(String const& method, String const& url, bool async, Optional<String> const& username = Optional<String> {}, Optional<String> const& password = Optional<String> {});
    WebIDL::ExceptionOr<void> send(Optional<DocumentOrXMLHttpRequestBodyInit> body);

    WebIDL::ExceptionOr<void> set_request_header(String const& header, String const& value);
    WebIDL::ExceptionOr<void> set_response_type(Bindings::XMLHttpRequestResponseType);

    WebIDL::ExceptionOr<Optional<String>> get_response_header(String const& name) const;
    WebIDL::ExceptionOr<String> get_all_response_headers() const;

    WebIDL::CallbackType* onreadystatechange();
    void set_onreadystatechange(WebIDL::CallbackType*);

    WebIDL::ExceptionOr<void> override_mime_type(String const& mime);

    u32 timeout() const;
    WebIDL::ExceptionOr<void> set_timeout(u32 timeout);

    bool with_credentials() const;
    WebIDL::ExceptionOr<void> set_with_credentials(bool);

    void abort();

    JS::NonnullGCPtr<XMLHttpRequestUpload> upload() const;

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual bool must_survive_garbage_collection() const override;

    [[nodiscard]] MimeSniff::MimeType get_response_mime_type() const;
    [[nodiscard]] Optional<StringView> get_final_encoding() const;
    [[nodiscard]] MimeSniff::MimeType get_final_mime_type() const;

    String get_text_response() const;
    void set_document_response();

    WebIDL::ExceptionOr<void> handle_response_end_of_body();
    WebIDL::ExceptionOr<void> handle_errors();
    JS::ThrowCompletionOr<void> request_error_steps(FlyString const& event_name, JS::GCPtr<WebIDL::DOMException> exception = nullptr);

    XMLHttpRequest(JS::Realm&, XMLHttpRequestUpload&, Fetch::Infrastructure::HeaderList&, Fetch::Infrastructure::Response&, Fetch::Infrastructure::FetchController&);

    // https://xhr.spec.whatwg.org/#upload-object
    // upload object
    //     An XMLHttpRequestUpload object.
    JS::NonnullGCPtr<XMLHttpRequestUpload> m_upload_object;

    // https://xhr.spec.whatwg.org/#concept-xmlhttprequest-state
    // state
    //     One of unsent, opened, headers received, loading, and done; initially unsent.
    State m_state { State::Unsent };

    // https://xhr.spec.whatwg.org/#send-flag
    // send() flag
    //     A flag, initially unset.
    bool m_send { false };

    // https://xhr.spec.whatwg.org/#timeout
    // timeout
    //     An unsigned integer, initially 0.
    u32 m_timeout { 0 };

    // https://xhr.spec.whatwg.org/#cross-origin-credentials
    // cross-origin credentials
    //     A boolean, initially false.
    bool m_cross_origin_credentials { false };

    // https://xhr.spec.whatwg.org/#request-method
    // request method
    //     A method.
    ByteString m_request_method;

    // https://xhr.spec.whatwg.org/#request-url
    // request URL
    //     A URL.
    URL::URL m_request_url;

    // https://xhr.spec.whatwg.org/#author-request-headers
    // author request headers
    //     A header list, initially empty.
    JS::NonnullGCPtr<Fetch::Infrastructure::HeaderList> m_author_request_headers;

    // https://xhr.spec.whatwg.org/#request-body
    // request body
    //     Initially null.
    JS::GCPtr<Fetch::Infrastructure::Body> m_request_body;

    // https://xhr.spec.whatwg.org/#synchronous-flag
    // synchronous flag
    //     A flag, initially unset.
    bool m_synchronous { false };

    // https://xhr.spec.whatwg.org/#upload-complete-flag
    // upload complete flag
    //     A flag, initially unset.
    bool m_upload_complete { false };

    // https://xhr.spec.whatwg.org/#upload-listener-flag
    // upload listener flag
    //     A flag, initially unset.
    bool m_upload_listener { false };

    // https://xhr.spec.whatwg.org/#timed-out-flag
    // timed out flag
    //     A flag, initially unset.
    bool m_timed_out { false };

    // https://xhr.spec.whatwg.org/#response
    // response
    //     A response, initially a network error.
    JS::NonnullGCPtr<Fetch::Infrastructure::Response> m_response;

    // https://xhr.spec.whatwg.org/#received-bytes
    // received bytes
    //     A byte sequence, initially the empty byte sequence.
    ByteBuffer m_received_bytes;

    // https://xhr.spec.whatwg.org/#response-type
    // response type
    //     One of the empty string, "arraybuffer", "blob", "document", "json", and "text"; initially the empty string.
    Bindings::XMLHttpRequestResponseType m_response_type;

    enum class Failure {
        /// ????
    };

    // https://xhr.spec.whatwg.org/#response-object
    // response object
    //     An object, failure, or null, initially null.
    //     NOTE: This needs to be a JS::Value as the JSON response might not actually be an object.
    Variant<JS::NonnullGCPtr<JS::Object>, Failure, Empty> m_response_object;

    // https://xhr.spec.whatwg.org/#xmlhttprequest-fetch-controller
    // fetch controller
    //     A fetch controller, initially a new fetch controller.
    //     NOTE: The send() method sets it to a useful fetch controller, but for simplicity it always holds a fetch controller.
    JS::NonnullGCPtr<Fetch::Infrastructure::FetchController> m_fetch_controller;

    // https://xhr.spec.whatwg.org/#override-mime-type
    // override MIME type
    //     A MIME type or null, initially null.
    //     NOTE: Can get a value when overrideMimeType() is invoked.
    Optional<MimeSniff::MimeType> m_override_mime_type;

    // Non-standard, see async path in `send()`
    u64 m_request_body_transmitted { 0 };
};

}
