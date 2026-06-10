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
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibHTTP/HeaderList.h>
#include <LibJS/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/XMLHttpRequest.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XHR/XMLHttpRequestEventTarget.h>

namespace Web::FileAPI {

class Blob;

}

namespace Web::XHR {

// https://fetch.spec.whatwg.org/#typedefdef-xmlhttprequestbodyinit
using DocumentOrXMLHttpRequestBodyInit = FlattenVariant<Variant<GC::Ref<Web::DOM::Document>>, Fetch::XMLHttpRequestBodyInit>;
using NullableDocumentOrXMLHttpRequestBodyInit = FlattenVariant<DocumentOrXMLHttpRequestBodyInit, Variant<Empty>>;

class XMLHttpRequest final : public XMLHttpRequestEventTarget {
    WEB_WRAPPABLE(XMLHttpRequest, XMLHttpRequestEventTarget);
    GC_DECLARE_ALLOCATOR(XMLHttpRequest);

public:
    static constexpr bool OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION = true;

    enum class State : u16 {
        Unsent = 0,
        Opened = 1,
        HeadersReceived = 2,
        Loading = 3,
        Done = 4,
    };

    using ResponseType = Bindings::XMLHttpRequestResponseType;

    static GC::Ref<XMLHttpRequest> create(GC::Ref<DOM::EventTarget> relevant_global_object);
    static WebIDL::ExceptionOr<GC::Ref<XMLHttpRequest>> create_for_constructor(JS::Realm&);

    virtual ~XMLHttpRequest() override;

    State ready_state() const { return m_state; }
    Fetch::Infrastructure::Status status() const;
    WebIDL::ExceptionOr<String> status_text() const;
    WebIDL::ExceptionOr<JS::Value> response(JS::Realm&);
    WebIDL::ExceptionOr<String> response_text() const;
    WebIDL::ExceptionOr<GC::Ptr<DOM::Document>> response_xml();
    ResponseType response_type() const { return m_response_type; }
    String response_url();

    WebIDL::ExceptionOr<void> open(String const& method, String const& url);
    WebIDL::ExceptionOr<void> open(String const& method, String const& url, bool async, Optional<String> const& username = Optional<String> {}, Optional<String> const& password = Optional<String> {});
    WebIDL::ExceptionOr<void> send(JS::Realm&, NullableDocumentOrXMLHttpRequestBodyInit body);

    WebIDL::ExceptionOr<void> set_request_header(String const& name, String const& value);
    WebIDL::ExceptionOr<void> set_response_type(ResponseType);

    Optional<String> get_response_header(String const& name) const;
    String get_all_response_headers() const;

    WebIDL::CallbackType* onreadystatechange();
    void set_onreadystatechange(WebIDL::CallbackType*);

    WebIDL::ExceptionOr<void> override_mime_type(String const& mime);

    u32 timeout() const;
    WebIDL::ExceptionOr<void> set_timeout(u32 timeout);

    bool with_credentials() const;
    WebIDL::ExceptionOr<void> set_with_credentials(bool);

    void abort();

    GC::Ref<XMLHttpRequestUpload> upload() const;
    String get_text_response() const;
    ByteBuffer const& received_bytes() const { return m_received_bytes; }
    bool response_body_is_null() const;
    bool response_object_is_failure() const { return m_response_object.has<Failure>(); }
    GC::Ptr<DOM::Document> response_document() const;
    GC::Ptr<FileAPI::Blob> response_blob() const;
    void set_blob_response_object(GC::Ref<FileAPI::Blob>);
    void set_document_response();
    MimeSniff::MimeType get_final_mime_type() const;
    u64 response_object_revision() const { return m_response_object_revision; }

private:
    virtual void visit_edges(Cell::Visitor&) override;
    virtual bool must_survive_garbage_collection() const override;

    [[nodiscard]] MimeSniff::MimeType get_response_mime_type() const;
    [[nodiscard]] Optional<StringView> get_final_encoding() const;

    JS::Object& relevant_global_object() const;
    GC::Ref<DOM::Event> create_associated_event(FlyString const&) const;
    HTML::EnvironmentSettingsObject& relevant_settings_object() const;
    void reset_response_object();

    WebIDL::ExceptionOr<void> handle_response_end_of_body();
    WebIDL::ExceptionOr<void> handle_errors();
    WebIDL::ExceptionOr<void> request_error_steps(FlyString const& event_name, GC::Ptr<WebIDL::DOMException> exception = nullptr);

    void stop_timeout_timer();

    XMLHttpRequest(GC::Ref<DOM::EventTarget> relevant_global_object, XMLHttpRequestUpload&, NonnullRefPtr<HTTP::HeaderList>, Fetch::Infrastructure::Response&, Fetch::Infrastructure::FetchController&);

    // https://xhr.spec.whatwg.org/#upload-object
    // upload object
    //     An XMLHttpRequestUpload object.
    GC::Ref<XMLHttpRequestUpload> m_upload_object;
    GC::Ref<DOM::EventTarget> m_global_object;

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
    NonnullRefPtr<HTTP::HeaderList> m_author_request_headers;

    // https://xhr.spec.whatwg.org/#request-body
    // request body
    //     Initially null.
    GC::Ptr<Fetch::Infrastructure::Body> m_request_body;

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
    GC::Ref<Fetch::Infrastructure::Response> m_response;

    // https://xhr.spec.whatwg.org/#received-bytes
    // received bytes
    //     A byte sequence, initially the empty byte sequence.
    ByteBuffer m_received_bytes;

    // https://xhr.spec.whatwg.org/#response-type
    // response type
    //     One of the empty string, "arraybuffer", "blob", "document", "json", and "text"; initially the empty string.
    ResponseType m_response_type { ResponseType::Empty };

    enum class Failure {
        /// ????
    };

    // https://xhr.spec.whatwg.org/#response-object
    // response object
    //     An object, failure, or null, initially null.
    Variant<GC::Ref<DOM::Document>, GC::Ref<FileAPI::Blob>, Failure, Empty> m_response_object;
    u64 m_response_object_revision { 0 };

    // https://xhr.spec.whatwg.org/#xmlhttprequest-fetch-controller
    // fetch controller
    //     A fetch controller, initially a new fetch controller.
    //     NOTE: The send() method sets it to a useful fetch controller, but for simplicity it always holds a fetch controller.
    GC::Ref<Fetch::Infrastructure::FetchController> m_fetch_controller;

    // https://xhr.spec.whatwg.org/#override-mime-type
    // override MIME type
    //     A MIME type or null, initially null.
    //     NOTE: Can get a value when overrideMimeType() is invoked.
    Optional<MimeSniff::MimeType> m_override_mime_type;

    // Non-standard, see async path in `send()`
    u64 m_request_body_transmitted { 0 };
    Optional<MonotonicTime> m_last_upload_progress_timestamp;
    Optional<MonotonicTime> m_last_download_progress_timestamp;

    GC::Ptr<Platform::Timer> m_timeout_timer;
};

}
