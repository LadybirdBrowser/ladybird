/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/QuickSort.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/XMLHttpRequestPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/XMLDocument.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Methods.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/ByteSequences.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XHR/EventNames.h>
#include <LibWeb/XHR/ProgressEvent.h>
#include <LibWeb/XHR/XMLHttpRequest.h>
#include <LibWeb/XHR/XMLHttpRequestUpload.h>

namespace Web::XHR {

GC_DEFINE_ALLOCATOR(XMLHttpRequest);

WebIDL::ExceptionOr<GC::Ref<XMLHttpRequest>> XMLHttpRequest::construct_impl(JS::Realm& realm)
{
    auto upload_object = realm.create<XMLHttpRequestUpload>(realm);
    auto author_request_headers = Fetch::Infrastructure::HeaderList::create(realm.vm());
    auto response = Fetch::Infrastructure::Response::network_error(realm.vm(), "Not sent yet"sv);
    auto fetch_controller = Fetch::Infrastructure::FetchController::create(realm.vm());
    return realm.create<XMLHttpRequest>(realm, *upload_object, *author_request_headers, *response, *fetch_controller);
}

XMLHttpRequest::XMLHttpRequest(JS::Realm& realm, XMLHttpRequestUpload& upload_object, Fetch::Infrastructure::HeaderList& author_request_headers, Fetch::Infrastructure::Response& response, Fetch::Infrastructure::FetchController& fetch_controller)
    : XMLHttpRequestEventTarget(realm)
    , m_upload_object(upload_object)
    , m_author_request_headers(author_request_headers)
    , m_response(response)
    , m_response_type(Bindings::XMLHttpRequestResponseType::Empty)
    , m_fetch_controller(fetch_controller)
{
    set_overrides_must_survive_garbage_collection(true);
}

XMLHttpRequest::~XMLHttpRequest() = default;

void XMLHttpRequest::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XMLHttpRequest);
}

void XMLHttpRequest::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_upload_object);
    visitor.visit(m_author_request_headers);
    visitor.visit(m_request_body);
    visitor.visit(m_response);
    visitor.visit(m_fetch_controller);

    if (auto* value = m_response_object.get_pointer<GC::Ref<JS::Object>>())
        visitor.visit(*value);
}

// https://xhr.spec.whatwg.org/#concept-event-fire-progress
static void fire_progress_event(XMLHttpRequestEventTarget& target, FlyString const& event_name, u64 transmitted, u64 length)
{
    // To fire a progress event named e at target, given transmitted and length, means to fire an event named e at target, using ProgressEvent,
    // with the loaded attribute initialized to transmitted, and if length is not 0, with the lengthComputable attribute initialized to true
    // and the total attribute initialized to length.
    ProgressEventInit event_init {};
    event_init.length_computable = length;
    event_init.loaded = transmitted;
    event_init.total = length;
    // FIXME: If we're in an async context, this will propagate to a callback context which can't propagate it anywhere else and does not expect this to fail.
    target.dispatch_event(*ProgressEvent::create(target.realm(), event_name, event_init));
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsetext
WebIDL::ExceptionOr<String> XMLHttpRequest::response_text() const
{
    // 1. If this’s response type is not the empty string or "text", then throw an "InvalidStateError" DOMException.
    if (m_response_type != Bindings::XMLHttpRequestResponseType::Empty && m_response_type != Bindings::XMLHttpRequestResponseType::Text)
        return WebIDL::InvalidStateError::create(realm(), "XHR responseText can only be used for responseType \"\" or \"text\""_string);

    // 2. If this’s state is not loading or done, then return the empty string.
    if (m_state != State::Loading && m_state != State::Done)
        return String {};

    // 3. Return the result of getting a text response for this.
    return get_text_response();
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsexml
WebIDL::ExceptionOr<GC::Ptr<DOM::Document>> XMLHttpRequest::response_xml()
{
    // 1. If this’s response type is not the empty string or "document", then throw an "InvalidStateError" DOMException.
    if (m_response_type != Bindings::XMLHttpRequestResponseType::Empty && m_response_type != Bindings::XMLHttpRequestResponseType::Document)
        return WebIDL::InvalidStateError::create(realm(), "XHR responseXML can only be used for responseXML \"\" or \"document\""_string);

    // 2. If this’s state is not done, then return null.
    if (m_state != State::Done)
        return nullptr;

    // 3. Assert: this’s response object is not failure.
    VERIFY(!m_response_object.has<Failure>());

    // 4. If this’s response object is non-null, then return it.
    if (!m_response_object.has<Empty>())
        return &as<DOM::Document>(*m_response_object.get<GC::Ref<JS::Object>>());

    // 5. Set a document response for this.
    set_document_response();

    // 6. Return this’s response object.
    if (m_response_object.has<Empty>())
        return nullptr;
    return &as<DOM::Document>(*m_response_object.get<GC::Ref<JS::Object>>());
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsetype
WebIDL::ExceptionOr<void> XMLHttpRequest::set_response_type(Bindings::XMLHttpRequestResponseType response_type)
{
    // 1. If the current global object is not a Window object and the given value is "document", then return.
    if (!is<HTML::Window>(HTML::current_principal_global_object()) && response_type == Bindings::XMLHttpRequestResponseType::Document)
        return {};

    // 2. If this’s state is loading or done, then throw an "InvalidStateError" DOMException.
    if (m_state == State::Loading || m_state == State::Done)
        return WebIDL::InvalidStateError::create(realm(), "Can't readyState when XHR is loading or done"_string);

    // 3. If the current global object is a Window object and this’s synchronous flag is set, then throw an "InvalidAccessError" DOMException.
    if (is<HTML::Window>(HTML::current_principal_global_object()) && m_synchronous)
        return WebIDL::InvalidAccessError::create(realm(), "Can't set readyState on synchronous XHR in Window environment"_string);

    // 4. Set this’s response type to the given value.
    m_response_type = response_type;
    return {};
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-response
WebIDL::ExceptionOr<JS::Value> XMLHttpRequest::response()
{
    auto& vm = this->vm();

    // 1. If this’s response type is the empty string or "text", then:
    if (m_response_type == Bindings::XMLHttpRequestResponseType::Empty || m_response_type == Bindings::XMLHttpRequestResponseType::Text) {
        // 1. If this’s state is not loading or done, then return the empty string.
        if (m_state != State::Loading && m_state != State::Done)
            return JS::PrimitiveString::create(vm, String {});

        // 2. Return the result of getting a text response for this.
        return JS::PrimitiveString::create(vm, get_text_response());
    }
    // 2. If this’s state is not done, then return null.
    if (m_state != State::Done)
        return JS::js_null();

    // 3. If this’s response object is failure, then return null.
    if (m_response_object.has<Failure>())
        return JS::js_null();

    // 4. If this’s response object is non-null, then return it.
    if (!m_response_object.has<Empty>())
        return m_response_object.get<GC::Ref<JS::Object>>();

    // 5. If this’s response type is "arraybuffer",
    if (m_response_type == Bindings::XMLHttpRequestResponseType::Arraybuffer) {
        // then set this’s response object to a new ArrayBuffer object representing this’s received bytes. If this throws an exception, then set this’s response object to failure and return null.
        auto buffer_result = JS::ArrayBuffer::create(realm(), m_received_bytes.size());
        if (buffer_result.is_error()) {
            m_response_object = Failure();
            return JS::js_null();
        }

        auto buffer = buffer_result.release_value();
        buffer->buffer().overwrite(0, m_received_bytes.data(), m_received_bytes.size());
        m_response_object = GC::Ref<JS::Object> { buffer };
    }
    // 6. Otherwise, if this’s response type is "blob", set this’s response object to a new Blob object representing this’s received bytes with type set to the result of get a final MIME type for this.
    else if (m_response_type == Bindings::XMLHttpRequestResponseType::Blob) {
        auto mime_type_as_string = get_final_mime_type().serialized();
        auto blob = FileAPI::Blob::create(realm(), m_received_bytes, move(mime_type_as_string));
        m_response_object = GC::Ref<JS::Object> { blob };
    }
    // 7. Otherwise, if this’s response type is "document", set a document response for this.
    else if (m_response_type == Bindings::XMLHttpRequestResponseType::Document) {
        set_document_response();
    }
    // 8. Otherwise:
    else {
        // 1. Assert: this’s response type is "json".
        // Note: Automatically done by the layers above us.

        // 2. If this’s response’s body is null, then return null.
        if (!m_response->body())
            return JS::js_null();

        // 3. Let jsonObject be the result of running parse JSON from bytes on this’s received bytes. If that threw an exception, then return null.
        auto json_object_result = Infra::parse_json_bytes_to_javascript_value(realm(), m_received_bytes);
        if (json_object_result.is_error())
            return JS::js_null();

        // 4. Set this’s response object to jsonObject.
        if (json_object_result.value().is_object())
            m_response_object = GC::Ref<JS::Object> { json_object_result.release_value().as_object() };
        else
            m_response_object = Empty {};
    }

    // 9. Return this’s response object.
    return m_response_object.visit(
        [](GC::Ref<JS::Object> object) -> JS::Value { return object; },
        [](auto const&) -> JS::Value { return JS::js_null(); });
}

// https://xhr.spec.whatwg.org/#text-response
String XMLHttpRequest::get_text_response() const
{
    // 1. If xhr’s response’s body is null, then return the empty string.
    if (!m_response->body())
        return String {};

    // 2. Let charset be the result of get a final encoding for xhr.
    auto charset = get_final_encoding();

    // 3. If xhr’s response type is the empty string, charset is null, and the result of get a final MIME type for xhr is an XML MIME type,
    if (m_response_type == Bindings::XMLHttpRequestResponseType::Empty && !charset.has_value() && get_final_mime_type().is_xml()) {
        // FIXME: then use the rules set forth in the XML specifications to determine the encoding. Let charset be the determined encoding. [XML] [XML-NAMES]
    }

    // 4. If charset is null, then set charset to UTF-8.
    if (!charset.has_value())
        charset = "UTF-8"sv;

    // 5. Return the result of running decode on xhr’s received bytes using fallback encoding charset.
    auto decoder = TextCodec::decoder_for(charset.value());

    // If we don't support the decoder yet, let's crash instead of attempting to return something, as the result would be incorrect and create obscure bugs.
    VERIFY(decoder.has_value());

    return TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, m_received_bytes).release_value_but_fixme_should_propagate_errors();
}

// https://xhr.spec.whatwg.org/#document-response
void XMLHttpRequest::set_document_response()
{
    // 1. If xhr’s response’s body is null, then return.
    if (!m_response->body())
        return;

    // 2. Let finalMIME be the result of get a final MIME type for xhr.
    auto final_mime = get_final_mime_type();

    // 3. If finalMIME is not an HTML MIME type or an XML MIME type, then return.
    if (!final_mime.is_html() && !final_mime.is_xml())
        return;

    // 4. If xhr’s response type is the empty string and finalMIME is an HTML MIME type, then return.
    if (m_response_type == Bindings::XMLHttpRequestResponseType::Empty && final_mime.is_html())
        return;

    // 5. If finalMIME is an HTML MIME type, then:
    Optional<String> charset;
    GC::Ptr<DOM::Document> document;
    if (final_mime.is_html()) {
        // 5.1. Let charset be the result of get a final encoding for xhr.
        if (auto final_encoding = get_final_encoding(); final_encoding.has_value())
            charset = MUST(String::from_utf8(*final_encoding));

        // 5.2. If charset is null, prescan the first 1024 bytes of xhr’s received bytes and if that does not terminate unsuccessfully then let charset be the return value.
        document = DOM::Document::create(realm());
        if (!charset.has_value())
            if (auto found_charset = HTML::run_prescan_byte_stream_algorithm(*document, m_received_bytes); found_charset.has_value())
                charset = MUST(String::from_byte_string(found_charset.value()));

        // 5.3. If charset is null, then set charset to UTF-8.
        if (!charset.has_value())
            charset = "UTF-8"_string;

        // 5.4. Let document be a document that represents the result parsing xhr’s received bytes following the rules set forth in the HTML Standard for an HTML parser with scripting disabled and a known definite encoding charset.
        auto parser = HTML::HTMLParser::create(*document, m_received_bytes, charset.value());
        parser->run(document->url());

        // 5.5. Flag document as an HTML document.
        document->set_document_type(DOM::Document::Type::HTML);
    }

    // 6. Otherwise, let document be a document that represents the result of running the XML parser with XML scripting support disabled on xhr’s received bytes. If that fails (unsupported character encoding, namespace well-formedness error, etc.), then return null.
    else {
        document = DOM::XMLDocument::create(realm(), m_response->url().value_or({}));
        if (!Web::build_xml_document(*document, m_received_bytes, {})) {
            m_response_object = Empty {};
            return;
        }
    }

    // 7. If charset is null, then set charset to UTF-8.
    if (!charset.has_value())
        charset = "UTF-8"_string;

    // 8. Set document’s encoding to charset.
    document->set_encoding(move(charset));

    // 9. Set document’s content type to finalMIME.
    document->set_content_type(final_mime.serialized());

    // 10. Set document’s URL to xhr’s response’s URL.
    document->set_url(m_response->url().value_or({}));

    // 11. Set document’s origin to xhr’s relevant settings object’s origin.
    document->set_origin(HTML::relevant_settings_object(*this).origin());

    // 12. Set xhr’s response object to document.
    m_response_object = GC::Ref<JS::Object> { *document };
}

// https://xhr.spec.whatwg.org/#final-mime-type
MimeSniff::MimeType XMLHttpRequest::get_final_mime_type() const
{
    // 1. If xhr’s override MIME type is null, return the result of get a response MIME type for xhr.
    if (!m_override_mime_type.has_value())
        return get_response_mime_type();

    // 2. Return xhr’s override MIME type.
    return *m_override_mime_type;
}

// https://xhr.spec.whatwg.org/#response-mime-type
MimeSniff::MimeType XMLHttpRequest::get_response_mime_type() const
{
    // 1. Let mimeType be the result of extracting a MIME type from xhr’s response’s header list.
    auto mime_type = m_response->header_list()->extract_mime_type();

    // 2. If mimeType is failure, then set mimeType to text/xml.
    if (!mime_type.has_value())
        return MimeSniff::MimeType::create("text"_string, "xml"_string);

    // 3. Return mimeType.
    return mime_type.release_value();
}

// https://xhr.spec.whatwg.org/#final-charset
Optional<StringView> XMLHttpRequest::get_final_encoding() const
{
    // 1. Let label be null.
    Optional<String> label;

    // 2. Let responseMIME be the result of get a response MIME type for xhr.
    auto response_mime = get_response_mime_type();

    // 3. If responseMIME’s parameters["charset"] exists, then set label to it.
    auto response_mime_charset_it = response_mime.parameters().find("charset"sv);
    if (response_mime_charset_it != response_mime.parameters().end())
        label = response_mime_charset_it->value;

    // 4. If xhr’s override MIME type’s parameters["charset"] exists, then set label to it.
    if (m_override_mime_type.has_value()) {
        auto override_mime_charset_it = m_override_mime_type->parameters().find("charset"sv);
        if (override_mime_charset_it != m_override_mime_type->parameters().end())
            label = override_mime_charset_it->value;
    }

    // 5. If label is null, then return null.
    if (!label.has_value())
        return OptionalNone {};

    // 6. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label.value());

    // 7. If encoding is failure, then return null.
    // 8. Return encoding.
    return encoding;
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-setrequestheader
WebIDL::ExceptionOr<void> XMLHttpRequest::set_request_header(String const& name_string, String const& value_string)
{
    auto& realm = this->realm();

    auto name = name_string.bytes();
    auto value = value_string.bytes();

    // 1. If this’s state is not opened, then throw an "InvalidStateError" DOMException.
    if (m_state != State::Opened)
        return WebIDL::InvalidStateError::create(realm, "XHR readyState is not OPENED"_string);

    // 2. If this’s send() flag is set, then throw an "InvalidStateError" DOMException.
    if (m_send)
        return WebIDL::InvalidStateError::create(realm, "XHR send() flag is already set"_string);

    // 3. Normalize value.
    auto normalized_value = Fetch::Infrastructure::normalize_header_value(value);

    // 4. If name is not a header name or value is not a header value, then throw a "SyntaxError" DOMException.
    if (!Fetch::Infrastructure::is_header_name(name))
        return WebIDL::SyntaxError::create(realm, "Header name contains invalid characters."_string);
    if (!Fetch::Infrastructure::is_header_value(normalized_value))
        return WebIDL::SyntaxError::create(realm, "Header value contains invalid characters."_string);

    auto header = Fetch::Infrastructure::Header {
        .name = MUST(ByteBuffer::copy(name)),
        .value = move(normalized_value),
    };

    // 5. If (name, value) is a forbidden request-header, then return.
    if (Fetch::Infrastructure::is_forbidden_request_header(header))
        return {};

    // 6. Combine (name, value) in this’s author request headers.
    m_author_request_headers->combine(move(header));

    return {};
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-open
WebIDL::ExceptionOr<void> XMLHttpRequest::open(String const& method_string, String const& url)
{
    // 7. If the async argument is omitted, set async to true, and set username and password to null.
    return open(method_string, url, true, Optional<String> {}, Optional<String> {});
}

WebIDL::ExceptionOr<void> XMLHttpRequest::open(String const& method_string, String const& url, bool async, Optional<String> const& username, Optional<String> const& password)
{
    auto method = method_string.bytes();

    // 1. If this’s relevant global object is a Window object and its associated Document is not fully active, then throw an "InvalidStateError" DOMException.
    if (is<HTML::Window>(HTML::relevant_global_object(*this))) {
        auto const& window = static_cast<HTML::Window const&>(HTML::relevant_global_object(*this));
        if (!window.associated_document().is_fully_active())
            return WebIDL::InvalidStateError::create(realm(), "Invalid state: Window's associated document is not fully active."_string);
    }

    // 2. If method is not a method, then throw a "SyntaxError" DOMException.
    if (!Fetch::Infrastructure::is_method(method))
        return WebIDL::SyntaxError::create(realm(), "An invalid or illegal string was specified."_string);

    // 3. If method is a forbidden method, then throw a "SecurityError" DOMException.
    if (Fetch::Infrastructure::is_forbidden_method(method))
        return WebIDL::SecurityError::create(realm(), "Forbidden method, must not be 'CONNECT', 'TRACE', or 'TRACK'"_string);

    // 4. Normalize method.
    auto normalized_method = Fetch::Infrastructure::normalize_method(method);

    // 5. Let parsedURL be the result of parsing url with this’s relevant settings object’s API base URL and this’s relevant settings object’s API URL character encoding.
    auto& relevant_settings_object = HTML::relevant_settings_object(*this);
    auto api_base_url = relevant_settings_object.api_base_url();
    auto api_url_character_encoding = relevant_settings_object.api_url_character_encoding();
    auto parsed_url = DOMURL::parse(url, api_base_url, api_url_character_encoding);

    // 6. If parsedURL is failure, then throw a "SyntaxError" DOMException.
    if (!parsed_url.has_value())
        return WebIDL::SyntaxError::create(realm(), "Invalid URL"_string);

    // 7. If the async argument is omitted, set async to true, and set username and password to null.
    // NOTE: This is handled in the overload lacking the async argument.

    // 8. If parsedURL’s host is non-null, then:
    if (parsed_url->host().has_value()) {
        // 1. If the username argument is not null, set the username given parsedURL and username.
        if (username.has_value())
            parsed_url->set_username(username.value());
        // 2. If the password argument is not null, set the password given parsedURL and password.
        if (password.has_value())
            parsed_url->set_password(password.value());
    }

    // 9. If async is false, the current global object is a Window object, and either this’s timeout is
    //     not 0 or this’s response type is not the empty string, then throw an "InvalidAccessError" DOMException.
    if (!async
        && is<HTML::Window>(HTML::current_principal_global_object())
        && (m_timeout != 0 || m_response_type != Bindings::XMLHttpRequestResponseType::Empty)) {
        return WebIDL::InvalidAccessError::create(realm(), "Synchronous XMLHttpRequests in a Window context do not support timeout or a non-empty responseType"_string);
    }

    // 10. Terminate this’s fetch controller.
    // Spec Note: A fetch can be ongoing at this point.
    m_fetch_controller->terminate();

    // 11. Set variables associated with the object as follows:
    // Unset this’s send() flag.
    m_send = false;
    // Unset this’s upload listener flag.
    m_upload_listener = false;
    // Set this’s request method to method.
    m_request_method = normalized_method.span();
    // Set this’s request URL to parsedURL.
    m_request_url = parsed_url.release_value();
    // Set this’s synchronous flag if async is false; otherwise unset this’s synchronous flag.
    m_synchronous = !async;
    // Empty this’s author request headers.
    m_author_request_headers->clear();
    // Set this’s response to a network error.
    m_response = Fetch::Infrastructure::Response::network_error(realm().vm(), "Not yet sent"sv);
    // Set this’s received bytes to the empty byte sequence.
    m_received_bytes = {};
    // Set this’s response object to null.
    m_response_object = {};
    // Spec Note: Override MIME type is not overridden here as the overrideMimeType() method can be invoked before the open() method.

    // 12. If this’s state is not opened, then:
    if (m_state != State::Opened) {
        // 1. Set this’s state to opened.
        m_state = State::Opened;

        // 2. Fire an event named readystatechange at this.
        dispatch_event(DOM::Event::create(realm(), EventNames::readystatechange));
    }

    return {};
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
WebIDL::ExceptionOr<void> XMLHttpRequest::send(Optional<DocumentOrXMLHttpRequestBodyInit> body)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. If this’s state is not opened, then throw an "InvalidStateError" DOMException.
    if (m_state != State::Opened)
        return WebIDL::InvalidStateError::create(realm, "XHR readyState is not OPENED"_string);

    // 2. If this’s send() flag is set, then throw an "InvalidStateError" DOMException.
    if (m_send)
        return WebIDL::InvalidStateError::create(realm, "XHR send() flag is already set"_string);

    // 3. If this’s request method is `GET` or `HEAD`, then set body to null.
    if (m_request_method.is_one_of("GET"sv, "HEAD"sv))
        body = {};

    // 4. If body is not null, then:
    if (body.has_value()) {
        // 1. Let extractedContentType be null.
        Optional<ByteBuffer> extracted_content_type;

        // 2. If body is a Document, then set this’s request body to body, serialized, converted, and UTF-8 encoded.
        if (body->has<GC::Root<DOM::Document>>()) {
            auto string_serialized_document = TRY(body->get<GC::Root<DOM::Document>>().cell()->serialize_fragment(DOMParsing::RequireWellFormed::No));
            m_request_body = Fetch::Infrastructure::byte_sequence_as_body(realm, string_serialized_document.bytes());
        }
        // 3. Otherwise:
        else {
            // 1. Let bodyWithType be the result of safely extracting body.
            auto body_with_type = Fetch::safely_extract_body(realm, body->downcast<Fetch::BodyInitOrReadableBytes>());

            // 2. Set this’s request body to bodyWithType’s body.
            m_request_body = body_with_type.body;

            // 3. Set extractedContentType to bodyWithType’s type.
            extracted_content_type = move(body_with_type.type);
        }

        // 4. Let originalAuthorContentType be the result of getting `Content-Type` from this’s author request headers.
        auto original_author_content_type = m_author_request_headers->get("Content-Type"sv.bytes());

        // 5. If originalAuthorContentType is non-null, then:
        if (original_author_content_type.has_value()) {
            // 1. If body is a Document or a USVString, then:
            if (body->has<GC::Root<DOM::Document>>() || body->has<String>()) {
                // 1. Let contentTypeRecord be the result of parsing originalAuthorContentType.
                auto content_type_record = MimeSniff::MimeType::parse(original_author_content_type.value());

                // 2. If contentTypeRecord is not failure, contentTypeRecord’s parameters["charset"] exists, and parameters["charset"] is not an ASCII case-insensitive match for "UTF-8", then:
                if (content_type_record.has_value()) {
                    auto charset_parameter_iterator = content_type_record->parameters().find("charset"sv);
                    if (charset_parameter_iterator != content_type_record->parameters().end() && !Infra::is_ascii_case_insensitive_match(charset_parameter_iterator->value, "UTF-8"sv)) {
                        // 1. Set contentTypeRecord’s parameters["charset"] to "UTF-8".
                        content_type_record->set_parameter("charset"_string, "UTF-8"_string);

                        // 2. Let newContentTypeSerialized be the result of serializing contentTypeRecord.
                        auto new_content_type_serialized = content_type_record->serialized();

                        // 3. Set (`Content-Type`, newContentTypeSerialized) in this’s author request headers.
                        auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, new_content_type_serialized);
                        m_author_request_headers->set(move(header));
                    }
                }
            }
        }
        // 6. Otherwise:
        else {
            if (body->has<GC::Root<DOM::Document>>()) {
                auto document = body->get<GC::Root<DOM::Document>>();

                // NOTE: A document can only be an HTML document or XML document.
                // 1. If body is an HTML document, then set (`Content-Type`, `text/html;charset=UTF-8`) in this’s author request headers.
                if (document->is_html_document()) {
                    auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, "text/html;charset=UTF-8"sv);
                    m_author_request_headers->set(move(header));
                }
                // 2. Otherwise, if body is an XML document, set (`Content-Type`, `application/xml;charset=UTF-8`) in this’s author request headers.
                else if (document->is_xml_document()) {
                    auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, "application/xml;charset=UTF-8"sv);
                    m_author_request_headers->set(move(header));
                } else {
                    VERIFY_NOT_REACHED();
                }
            }
            // 3. Otherwise, if extractedContentType is not null, set (`Content-Type`, extractedContentType) in this’s author request headers.
            else if (extracted_content_type.has_value()) {
                auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, extracted_content_type.value());
                m_author_request_headers->set(move(header));
            }
        }
    }

    // 5. If one or more event listeners are registered on this’s upload object, then set this’s upload listener flag.
    m_upload_listener = m_upload_object->has_event_listeners();

    // 6. Let req be a new request, initialized as follows:
    auto request = Fetch::Infrastructure::Request::create(vm);

    // method
    //    This’s request method.
    request->set_method(MUST(ByteBuffer::copy(m_request_method.bytes())));

    // URL
    //    This’s request URL.
    request->set_url(m_request_url);

    // header list
    //    This’s author request headers.
    request->set_header_list(m_author_request_headers);

    // unsafe-request flag
    //    Set.
    request->set_unsafe_request(true);

    // body
    //    This’s request body.
    if (m_request_body)
        request->set_body(GC::Ref { *m_request_body });

    // client
    //    This’s relevant settings object.
    request->set_client(&HTML::relevant_settings_object(*this));

    // mode
    //    "cors".
    request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);

    // use-CORS-preflight flag
    //    Set if this’s upload listener flag is set.
    request->set_use_cors_preflight(m_upload_listener);

    // credentials mode
    //    If this’s cross-origin credentials is true, then "include"; otherwise "same-origin".
    request->set_credentials_mode(m_cross_origin_credentials ? Fetch::Infrastructure::Request::CredentialsMode::Include : Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);

    // use-URL-credentials flag
    //    Set if this’s request URL includes credentials.
    request->set_use_url_credentials(m_request_url.includes_credentials());

    // initiator type
    //    "xmlhttprequest".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::XMLHttpRequest);

    // 7. Unset this’s upload complete flag.
    m_upload_complete = false;

    // 8. Unset this’s timed out flag.
    m_timed_out = false;

    // 9. If req’s body is null, then set this’s upload complete flag.
    // NOTE: req's body is always m_request_body here, see step 6.
    if (!m_request_body)
        m_upload_complete = true;

    // 10. Set this’s send() flag.
    m_send = true;

    dbgln_if(SPAM_DEBUG, "{}XHR send from {} to {}", m_synchronous ? "\033[33;1mSynchronous\033[0m " : "", HTML::relevant_settings_object(*this).creation_url, m_request_url);

    // 11. If this’s synchronous flag is unset, then:
    if (!m_synchronous) {
        // 1. Fire a progress event named loadstart at this with 0 and 0.
        fire_progress_event(*this, EventNames::loadstart, 0, 0);

        // 2. Let requestBodyTransmitted be 0.
        // NOTE: This is kept on the XHR object itself instead of the stack, as we cannot capture references to stack variables in an async context.
        m_request_body_transmitted = 0;

        // 3. Let requestBodyLength be req’s body’s length, if req’s body is non-null; otherwise 0.
        // NOTE: req's body is always m_request_body here, see step 6.
        // 4. Assert: requestBodyLength is an integer.
        // NOTE: This is done to provide a better assertion failure message, whereas below the message would be "m_has_value"
        if (m_request_body)
            VERIFY(m_request_body->length().has_value());

        // NOTE: This is const to allow the callback functions to take a copy of it and know it won't change.
        auto const request_body_length = m_request_body ? m_request_body->length().value() : 0;

        // 5. If this’s upload complete flag is unset and this’s upload listener flag is set, then fire a progress event named loadstart at this’s upload object with requestBodyTransmitted and requestBodyLength.
        if (!m_upload_complete && m_upload_listener)
            fire_progress_event(m_upload_object, EventNames::loadstart, m_request_body_transmitted, request_body_length);

        // 6. If this’s state is not opened or this’s send() flag is unset, then return.
        if (m_state != State::Opened || !m_send)
            return {};

        // 7. Let processRequestBodyChunkLength, given a bytesLength, be these steps:
        // NOTE: request_body_length is captured by copy as to not UAF it when we leave `send()` and the callback gets called.
        // NOTE: `this` is kept alive by FetchAlgorithms using GC::HeapFunction.
        auto process_request_body_chunk_length = [this, request_body_length](u64 bytes_length) {
            // 1. Increase requestBodyTransmitted by bytesLength.
            m_request_body_transmitted += bytes_length;

            // FIXME: 2. If not roughly 50ms have passed since these steps were last invoked, then return.

            // 3. If this’s upload listener flag is set, then fire a progress event named progress at this’s upload object with requestBodyTransmitted and requestBodyLength.
            if (m_upload_listener)
                fire_progress_event(m_upload_object, EventNames::progress, m_request_body_transmitted, request_body_length);
        };

        // 8. Let processRequestEndOfBody be these steps:
        // NOTE: request_body_length is captured by copy as to not UAF it when we leave `send()` and the callback gets called.
        // NOTE: `this` is kept alive by FetchAlgorithms using GC::HeapFunction.
        auto process_request_end_of_body = [this, request_body_length]() {
            // 1. Set this’s upload complete flag.
            m_upload_complete = true;

            // 2. If this’s upload listener flag is unset, then return.
            if (!m_upload_listener)
                return;

            // 3. Fire a progress event named progress at this’s upload object with requestBodyTransmitted and requestBodyLength.
            fire_progress_event(m_upload_object, EventNames::progress, m_request_body_transmitted, request_body_length);

            // 4. Fire a progress event named load at this’s upload object with requestBodyTransmitted and requestBodyLength.
            fire_progress_event(m_upload_object, EventNames::load, m_request_body_transmitted, request_body_length);

            // 5. Fire a progress event named loadend at this’s upload object with requestBodyTransmitted and requestBodyLength.
            fire_progress_event(m_upload_object, EventNames::loadend, m_request_body_transmitted, request_body_length);
        };

        // 9. Let processResponse, given a response, be these steps:
        // NOTE: `this` is kept alive by FetchAlgorithms using GC::HeapFunction.
        auto process_response = [this](GC::Ref<Fetch::Infrastructure::Response> response) {
            // 1. Set this’s response to response.
            m_response = response;

            // 2. Handle errors for this.
            // NOTE: This cannot throw, as `handle_errors` only throws in a synchronous context.
            // FIXME: However, we can receive allocation failures, but we can't propagate them anywhere currently.
            handle_errors().release_value_but_fixme_should_propagate_errors();

            // 3. If this’s response is a network error, then return.
            if (m_response->is_network_error())
                return;

            // 4. Set this’s state to headers received.
            m_state = State::HeadersReceived;

            // 5. Fire an event named readystatechange at this.
            // FIXME: We're in an async context, so we can't propagate the error anywhere.
            dispatch_event(*DOM::Event::create(this->realm(), EventNames::readystatechange));

            // 6. If this’s state is not headers received, then return.
            if (m_state != State::HeadersReceived)
                return;

            // 7. If this’s response’s body is null, then run handle response end-of-body for this and return.
            if (!m_response->body()) {
                // NOTE: This cannot throw, as `handle_response_end_of_body` only throws in a synchronous context.
                // FIXME: However, we can receive allocation failures, but we can't propagate them anywhere currently.
                handle_response_end_of_body().release_value_but_fixme_should_propagate_errors();
                return;
            }

            // 8. Let length be the result of extracting a length from this’s response’s header list.
            // FIXME: We're in an async context, so we can't propagate the error anywhere.
            auto length = m_response->header_list()->extract_length();

            // 9. If length is not an integer, then set it to 0.
            if (!length.has<u64>())
                length = 0;

            // FIXME: We can't implement these steps yet, as we don't fully implement the Streams standard.

            // 10. Let processBodyChunk given bytes be these steps:
            auto process_body_chunks = GC::create_function(heap(), [this, length](ByteBuffer byte_buffer) {
                // 1. Append bytes to this’s received bytes.
                m_received_bytes.append(byte_buffer);

                // FIXME: 2. If not roughly 50ms have passed since these steps were last invoked, then return.

                // 3. If this’s state is headers received, then set this’s state to loading.
                if (m_state == State::HeadersReceived)
                    m_state = State::Loading;

                // 4. Fire an event named readystatechange at this.
                // Spec Note: Web compatibility is the reason readystatechange fires more often than this’s state changes.
                dispatch_event(*DOM::Event::create(this->realm(), EventNames::readystatechange));

                // 5. Fire a progress event named progress at this with this’s received bytes’s length and length.
                fire_progress_event(*this, EventNames::progress, m_received_bytes.size(), length.get<u64>());
            });

            // 11. Let processEndOfBody be this step: run handle response end-of-body for this.
            auto process_end_of_body = GC::create_function(heap(), [this]() {
                // NOTE: This cannot throw, as `handle_response_end_of_body` only throws in a synchronous context.
                // FIXME: However, we can receive allocation failures, but we can't propagate them anywhere currently.
                handle_response_end_of_body().release_value_but_fixme_should_propagate_errors();
            });

            // 12. Let processBodyError be these steps:
            auto process_body_error = GC::create_function(heap(), [this](JS::Value) {
                auto& vm = this->vm();
                // 1. Set this’s response to a network error.
                m_response = Fetch::Infrastructure::Response::network_error(vm, "A network error occurred processing body."sv);
                // 2. Run handle errors for this.
                // NOTE: This cannot throw, as `handle_errors` only throws in a synchronous context.
                // FIXME: However, we can receive allocation failures, but we can't propagate them anywhere currently.
                handle_errors().release_value_but_fixme_should_propagate_errors();
            });

            // 13. Incrementally read this’s response’s body, given processBodyChunk, processEndOfBody, processBodyError, and this’s relevant global object.
            auto global_object = GC::Ref<JS::Object> { HTML::relevant_global_object(*this) };
            response->body()->incrementally_read(process_body_chunks, process_end_of_body, process_body_error, global_object);
        };

        // 10. Set this’s fetch controller to the result of fetching req with processRequestBodyChunkLength set to processRequestBodyChunkLength, processRequestEndOfBody set to processRequestEndOfBody, and processResponse set to processResponse.
        m_fetch_controller = TRY(Fetch::Fetching::fetch(
            realm,
            request,
            Fetch::Infrastructure::FetchAlgorithms::create(vm,
                {
                    .process_request_body_chunk_length = move(process_request_body_chunk_length),
                    .process_request_end_of_body = move(process_request_end_of_body),
                    .process_early_hints_response = {},
                    .process_response = move(process_response),
                    .process_response_end_of_body = {},
                    .process_response_consume_body = {},
                })));

        // 11. Let now be the present time.
        // 12. Run these steps in parallel:
        //     1. Wait until either req’s done flag is set or this’s timeout is not 0 and this’s timeout milliseconds have passed since now.
        //     2. If req’s done flag is unset, then set this’s timed out flag and terminate this’s fetch controller.
        if (m_timeout != 0) {
            auto timer = Platform::Timer::create_single_shot(heap(), m_timeout, nullptr);

            // NOTE: `timer` is kept alive by capturing into the lambda for the GC to see
            // NOTE: `this` and `request` is kept alive by Platform::Timer using a Handle.
            timer->on_timeout = GC::create_function(heap(), [this, request, timer = GC::make_root(timer)]() {
                (void)timer;
                if (!request->done()) {
                    m_timed_out = true;
                    m_fetch_controller->terminate();
                }
            });

            timer->start();
        }
    } else {
        // 1. Let processedResponse be false.
        IGNORE_USE_IN_ESCAPING_LAMBDA bool processed_response = false;

        // 2. Let processResponseConsumeBody, given a response and nullOrFailureOrBytes, be these steps:
        auto process_response_consume_body = [this, &processed_response](GC::Ref<Fetch::Infrastructure::Response> response, Variant<Empty, Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag, ByteBuffer> null_or_failure_or_bytes) {
            // 1. If nullOrFailureOrBytes is not failure, then set this’s response to response.
            if (!null_or_failure_or_bytes.has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>())
                m_response = response;

            // 2. If nullOrFailureOrBytes is a byte sequence, then append nullOrFailureOrBytes to this’s received bytes.
            if (null_or_failure_or_bytes.has<ByteBuffer>()) {
                // NOTE: We are not in a context where we can throw if this fails due to OOM.
                m_received_bytes.append(null_or_failure_or_bytes.get<ByteBuffer>());
            }

            // 3. Set processedResponse to true.
            processed_response = true;
        };

        // 3. Set this’s fetch controller to the result of fetching req with processResponseConsumeBody set to processResponseConsumeBody and useParallelQueue set to true.
        m_fetch_controller = TRY(Fetch::Fetching::fetch(
            realm,
            request,
            Fetch::Infrastructure::FetchAlgorithms::create(vm,
                {
                    .process_request_body_chunk_length = {},
                    .process_request_end_of_body = {},
                    .process_early_hints_response = {},
                    .process_response = {},
                    .process_response_end_of_body = {},
                    .process_response_consume_body = move(process_response_consume_body),
                }),
            Fetch::Fetching::UseParallelQueue::Yes));

        // 4. Let now be the present time.
        // 5. Pause until either processedResponse is true or this’s timeout is not 0 and this’s timeout milliseconds have passed since now.
        IGNORE_USE_IN_ESCAPING_LAMBDA bool did_time_out = false;

        if (m_timeout != 0) {
            auto timer = Platform::Timer::create_single_shot(heap(), m_timeout, nullptr);

            // NOTE: `timer` is kept alive by capturing into the lambda for the GC to see
            timer->on_timeout = GC::create_function(heap(), [timer = GC::make_root(timer), &did_time_out]() {
                (void)timer;
                did_time_out = true;
            });

            timer->start();
        }

        // FIXME: This is not exactly correct, as it allows the HTML event loop to continue executing tasks.
        HTML::main_thread_event_loop().spin_until(GC::create_function(heap(), [&]() {
            return processed_response || did_time_out;
        }));

        // 6. If processedResponse is false, then set this’s timed out flag and terminate this’s fetch controller.
        if (!processed_response) {
            m_timed_out = true;
            m_fetch_controller->terminate();
        }

        // FIXME: 7. Report timing for this’s fetch controller given the current global object.
        //        We cannot do this for responses that have a body yet, as we do not setup the stream that then calls processResponseEndOfBody in `fetch_response_handover`.

        // 8. Run handle response end-of-body for this.
        TRY(handle_response_end_of_body());
    }
    return {};
}

WebIDL::CallbackType* XMLHttpRequest::onreadystatechange()
{
    return event_handler_attribute(Web::XHR::EventNames::readystatechange);
}

void XMLHttpRequest::set_onreadystatechange(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(Web::XHR::EventNames::readystatechange, value);
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-getresponseheader
Optional<String> XMLHttpRequest::get_response_header(String const& name) const
{
    // The getResponseHeader(name) method steps are to return the result of getting name from this’s response’s header list.
    auto header_bytes = m_response->header_list()->get(name.bytes());
    if (!header_bytes.has_value())
        return {};

    // FIXME: The spec doesn't mention isomorphic decode. Spec bug?
    return Infra::isomorphic_decode(header_bytes->bytes());
}

// https://xhr.spec.whatwg.org/#legacy-uppercased-byte-less-than
static ErrorOr<bool> is_legacy_uppercased_byte_less_than(ReadonlyBytes a, ReadonlyBytes b)
{
    // 1. Let A be a, byte-uppercased.
    auto uppercased_a = TRY(ByteBuffer::copy(a));
    Infra::byte_uppercase(uppercased_a);

    // 2. Let B be b, byte-uppercased.
    auto uppercased_b = TRY(ByteBuffer::copy(b));
    Infra::byte_uppercase(uppercased_b);

    // 3. Return A is byte less than B.
    return Infra::is_byte_less_than(uppercased_a, uppercased_b);
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-getallresponseheaders
String XMLHttpRequest::get_all_response_headers() const
{
    // 1. Let output be an empty byte sequence.
    StringBuilder output;

    // 2. Let initialHeaders be the result of running sort and combine with this’s response’s header list.
    auto initial_headers = m_response->header_list()->sort_and_combine();

    // 3. Let headers be the result of sorting initialHeaders in ascending order, with a being less than b if a’s name is legacy-uppercased-byte less than b’s name.
    // Spec Note: Unfortunately, this is needed for compatibility with deployed content.
    // NOTE: quick_sort mutates the collection instead of returning a sorted copy.
    quick_sort(initial_headers, [](Fetch::Infrastructure::Header const& a, Fetch::Infrastructure::Header const& b) {
        return MUST(is_legacy_uppercased_byte_less_than(a.name, b.name));
    });

    // 4. For each header in headers, append header’s name, followed by a 0x3A 0x20 byte pair, followed by header’s value, followed by a 0x0D 0x0A byte pair, to output.
    for (auto const& header : initial_headers) {
        output.append(header.name);
        output.append(0x3A); // ':'
        output.append(0x20); // ' '
        // FIXME: The spec does not mention isomorphic decode. Spec bug?
        output.append(Infra::isomorphic_decode(header.value).bytes());
        output.append(0x0D); // '\r'
        output.append(0x0A); // '\n'
    }

    // 5. Return output.
    return MUST(output.to_string());
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-overridemimetype
WebIDL::ExceptionOr<void> XMLHttpRequest::override_mime_type(String const& mime)
{
    // 1. If this’s state is loading or done, then throw an "InvalidStateError" DOMException.
    if (m_state == State::Loading || m_state == State::Done)
        return WebIDL::InvalidStateError::create(realm(), "Cannot override MIME type when state is Loading or Done."_string);

    // 2. Set this’s override MIME type to the result of parsing mime.
    m_override_mime_type = MimeSniff::MimeType::parse(mime);

    // 3. If this’s override MIME type is failure, then set this’s override MIME type to application/octet-stream.
    if (!m_override_mime_type.has_value())
        m_override_mime_type = MimeSniff::MimeType::create("application"_string, "octet-stream"_string);

    return {};
}

// https://xhr.spec.whatwg.org/#ref-for-dom-xmlhttprequest-timeout%E2%91%A2
WebIDL::ExceptionOr<void> XMLHttpRequest::set_timeout(u32 timeout)
{
    // 1. If the current global object is a Window object and this’s synchronous flag is set,
    //    then throw an "InvalidAccessError" DOMException.
    if (is<HTML::Window>(HTML::current_principal_global_object()) && m_synchronous)
        return WebIDL::InvalidAccessError::create(realm(), "Use of XMLHttpRequest's timeout attribute is not supported in the synchronous mode in window context."_string);

    // 2. Set this’s timeout to the given value.
    m_timeout = timeout;

    return {};
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-timeout
u32 XMLHttpRequest::timeout() const { return m_timeout; }

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-withcredentials
bool XMLHttpRequest::with_credentials() const
{
    // The withCredentials getter steps are to return this’s cross-origin credentials.
    return m_cross_origin_credentials;
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-withcredentials
WebIDL::ExceptionOr<void> XMLHttpRequest::set_with_credentials(bool with_credentials)
{
    auto& realm = this->realm();

    // 1. If this’s state is not unsent or opened, then throw an "InvalidStateError" DOMException.
    if (m_state != State::Unsent && m_state != State::Opened)
        return WebIDL::InvalidStateError::create(realm, "XHR readyState is not UNSENT or OPENED"_string);

    // 2. If this’s send() flag is set, then throw an "InvalidStateError" DOMException.
    if (m_send)
        return WebIDL::InvalidStateError::create(realm, "XHR send() flag is already set"_string);

    // 3. Set this’s cross-origin credentials to the given value.
    m_cross_origin_credentials = with_credentials;

    return {};
}

// https://xhr.spec.whatwg.org/#garbage-collection
bool XMLHttpRequest::must_survive_garbage_collection() const
{
    // An XMLHttpRequest object must not be garbage collected
    // if its state is either opened with the send() flag set, headers received, or loading,
    // and it has one or more event listeners registered whose type is one of
    // readystatechange, progress, abort, error, load, timeout, and loadend.
    if ((m_state == State::Opened && m_send)
        || m_state == State::HeadersReceived
        || m_state == State::Loading) {
        if (has_event_listener(EventNames::readystatechange))
            return true;
        if (has_event_listener(EventNames::progress))
            return true;
        if (has_event_listener(EventNames::abort))
            return true;
        if (has_event_listener(EventNames::error))
            return true;
        if (has_event_listener(EventNames::load))
            return true;
        if (has_event_listener(EventNames::timeout))
            return true;
        if (has_event_listener(EventNames::loadend))
            return true;
    }

    // FIXME: If an XMLHttpRequest object is garbage collected while its connection is still open,
    //        the user agent must terminate the XMLHttpRequest object’s fetch controller.
    // NOTE: This would go in XMLHttpRequest::finalize().

    return false;
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-abort
void XMLHttpRequest::abort()
{
    // 1. Abort this’s fetch controller.
    m_fetch_controller->abort(realm(), {});

    // 2. If this’s state is opened with this’s send() flag set, headers received, or loading, then run the request error steps for this and abort.
    if ((m_state == State::Opened || m_state == State::HeadersReceived || m_state == State::Loading) && m_send) {
        // NOTE: This cannot throw as we don't pass in an exception. XHR::abort cannot be reached in a synchronous context where the state matches above.
        //       This is because it pauses inside XHR::send until the request is done or times out and then immediately calls `handle_response_end_of_body`
        //       which will always set `m_state` to `Done`.
        MUST(request_error_steps(EventNames::abort));
    }

    // 3. If this’s state is done, then set this’s state to unsent and this’s response to a network error.
    // Spec Note: No readystatechange event is dispatched.
    if (m_state == State::Done) {
        m_state = State::Unsent;
        m_response = Fetch::Infrastructure::Response::network_error(vm(), "Not yet sent"sv);
    }
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-upload
GC::Ref<XMLHttpRequestUpload> XMLHttpRequest::upload() const
{
    // The upload getter steps are to return this’s upload object.
    return m_upload_object;
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-status
Fetch::Infrastructure::Status XMLHttpRequest::status() const
{
    // The status getter steps are to return this’s response’s status.
    return m_response->status();
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-statustext
WebIDL::ExceptionOr<String> XMLHttpRequest::status_text() const
{
    auto& vm = this->vm();

    // The statusText getter steps are to return this’s response’s status message.
    return TRY_OR_THROW_OOM(vm, String::from_utf8(m_response->status_message()));
}

// https://xhr.spec.whatwg.org/#handle-response-end-of-body
WebIDL::ExceptionOr<void> XMLHttpRequest::handle_response_end_of_body()
{
    auto& realm = this->realm();

    // 1. Handle errors for xhr.
    TRY(handle_errors());

    // 2. If xhr’s response is a network error, then return.
    if (m_response->is_network_error())
        return {};

    // 3. Let transmitted be xhr’s received bytes’s length.
    auto transmitted = m_received_bytes.size();

    // 4. Let length be the result of extracting a length from this’s response’s header list.
    auto maybe_length = m_response->header_list()->extract_length();

    // 5. If length is not an integer, then set it to 0.
    if (!maybe_length.has<u64>())
        maybe_length = 0;

    auto length = maybe_length.get<u64>();

    // 6. If xhr’s synchronous flag is unset, then fire a progress event named progress at xhr with transmitted and length.
    if (!m_synchronous)
        fire_progress_event(*this, EventNames::progress, transmitted, length);

    // 7. Set xhr’s state to done.
    m_state = State::Done;

    // 8. Unset xhr’s send() flag.
    m_send = false;

    // 9. Fire an event named readystatechange at xhr.
    // FIXME: If we're in an async context, this will propagate to a callback context which can't propagate it anywhere else and does not expect this to fail.
    dispatch_event(*DOM::Event::create(realm, EventNames::readystatechange));

    // 10. Fire a progress event named load at xhr with transmitted and length.
    fire_progress_event(*this, EventNames::load, transmitted, length);

    // 11. Fire a progress event named loadend at xhr with transmitted and length.
    fire_progress_event(*this, EventNames::loadend, transmitted, length);

    return {};
}

// https://xhr.spec.whatwg.org/#handle-errors
WebIDL::ExceptionOr<void> XMLHttpRequest::handle_errors()
{
    // 1. If xhr’s send() flag is unset, then return.
    if (!m_send)
        return {};

    // 2. If xhr’s timed out flag is set, then run the request error steps for xhr, timeout, and "TimeoutError" DOMException.
    if (m_timed_out)
        return TRY(request_error_steps(EventNames::timeout, WebIDL::TimeoutError::create(realm(), "Timed out"_string)));

    // 3. Otherwise, if xhr’s response’s aborted flag is set, run the request error steps for xhr, abort, and "AbortError" DOMException.
    if (m_response->aborted())
        return TRY(request_error_steps(EventNames::abort, WebIDL::AbortError::create(realm(), "Aborted"_string)));

    // 4. Otherwise, if xhr’s response is a network error, then run the request error steps for xhr, error, and "NetworkError" DOMException.
    if (m_response->is_network_error())
        return TRY(request_error_steps(EventNames::error, WebIDL::NetworkError::create(realm(), "Network error"_string)));

    return {};
}

JS::ThrowCompletionOr<void> XMLHttpRequest::request_error_steps(FlyString const& event_name, GC::Ptr<WebIDL::DOMException> exception)
{
    // 1. Set xhr’s state to done.
    m_state = State::Done;

    // 2. Unset xhr’s send() flag.
    m_send = false;

    // 3. Set xhr’s response to a network error.
    m_response = Fetch::Infrastructure::Response::network_error(realm().vm(), "Failed to load"sv);

    // 4. If xhr’s synchronous flag is set, then throw exception.
    if (m_synchronous) {
        VERIFY(exception);
        return JS::throw_completion(exception.ptr());
    }

    // 5. Fire an event named readystatechange at xhr.
    // FIXME: Since we're in an async context, this will propagate to a callback context which can't propagate it anywhere else and does not expect this to fail.
    dispatch_event(*DOM::Event::create(realm(), EventNames::readystatechange));

    // 6. If xhr’s upload complete flag is unset, then:
    if (!m_upload_complete) {
        // 1. Set xhr’s upload complete flag.
        m_upload_complete = true;

        // 2. If xhr’s upload listener flag is set, then:
        if (m_upload_listener) {
            // 1. Fire a progress event named event at xhr’s upload object with 0 and 0.
            fire_progress_event(m_upload_object, event_name, 0, 0);

            // 2. Fire a progress event named loadend at xhr’s upload object with 0 and 0.
            fire_progress_event(m_upload_object, EventNames::loadend, 0, 0);
        }
    }

    // 7. Fire a progress event named event at xhr with 0 and 0.
    fire_progress_event(*this, event_name, 0, 0);

    // 8. Fire a progress event named loadend at xhr with 0 and 0.
    fire_progress_event(*this, EventNames::loadend, 0, 0);

    return {};
}

// https://xhr.spec.whatwg.org/#the-responseurl-attribute
String XMLHttpRequest::response_url()
{
    // The responseURL getter steps are to return the empty string if this’s response’s URL is null;
    // otherwise its serialization with the exclude fragment flag set.
    if (!m_response->url().has_value())
        return String {};

    auto serialized = m_response->url().value().serialize(URL::ExcludeFragment::Yes);
    return String::from_utf8_without_validation(serialized.bytes());
}

}
