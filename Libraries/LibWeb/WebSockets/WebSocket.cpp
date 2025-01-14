/*
 * Copyright (c) 2021-2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/WebSocketPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/CloseEvent.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Web::WebSockets {

GC_DEFINE_ALLOCATOR(WebSocket);

// https://websockets.spec.whatwg.org/#dom-websocket-websocket
WebIDL::ExceptionOr<GC::Ref<WebSocket>> WebSocket::construct_impl(JS::Realm& realm, String const& url, Optional<Variant<String, Vector<String>>> const& protocols)
{
    auto& vm = realm.vm();

    auto web_socket = realm.create<WebSocket>(realm);
    auto& relevant_settings_object = HTML::relevant_settings_object(*web_socket);

    // 1. Let baseURL be this's relevant settings object's API base URL.
    auto base_url = relevant_settings_object.api_base_url();

    // 2. Let urlRecord be the result of applying the URL parser to url with baseURL.
    auto url_record = DOMURL::parse(url, base_url);

    // 3. If urlRecord is failure, then throw a "SyntaxError" DOMException.
    if (!url_record.has_value())
        return WebIDL::SyntaxError::create(realm, "Invalid URL"_string);

    // 4. If urlRecord’s scheme is "http", then set urlRecord’s scheme to "ws".
    if (url_record->scheme() == "http"sv)
        url_record->set_scheme("ws"_string);
    // 5. Otherwise, if urlRecord’s scheme is "https", set urlRecord’s scheme to "wss".
    else if (url_record->scheme() == "https"sv)
        url_record->set_scheme("wss"_string);

    // 6. If urlRecord’s scheme is not "ws" or "wss", then throw a "SyntaxError" DOMException.
    if (!url_record->scheme().is_one_of("ws"sv, "wss"sv))
        return WebIDL::SyntaxError::create(realm, "Invalid protocol"_string);

    // 7. If urlRecord’s fragment is non-null, then throw a "SyntaxError" DOMException.
    if (url_record->fragment().has_value())
        return WebIDL::SyntaxError::create(realm, "Presence of URL fragment is invalid"_string);

    Vector<String> protocols_sequence;
    // 8. If protocols is a string, set protocols to a sequence consisting of just that string.
    if (protocols.has_value() && protocols->has<String>())
        protocols_sequence = { protocols.value().get<String>() };
    else if (protocols.has_value() && protocols->has<Vector<String>>())
        protocols_sequence = protocols.value().get<Vector<String>>();
    else
        protocols_sequence = {};

    // 9. If any of the values in protocols occur more than once or otherwise fail to match the requirements for elements that comprise
    //    the value of `Sec-WebSocket-Protocol` fields as defined by The WebSocket protocol, then throw a "SyntaxError" DOMException. [WSP]
    auto sorted_protocols = protocols_sequence;
    quick_sort(sorted_protocols);
    for (size_t i = 0; i < sorted_protocols.size(); i++) {
        // https://datatracker.ietf.org/doc/html/rfc6455
        // The elements that comprise this value MUST be non-empty strings with characters in the range U+0021 to U+007E not including
        // separator characters as defined in [RFC2616] and MUST all be unique strings.
        auto protocol = sorted_protocols[i];
        if (i < sorted_protocols.size() - 1 && protocol == sorted_protocols[i + 1])
            return WebIDL::SyntaxError::create(realm, "Found a duplicate protocol name in the specified list"_string);
        for (auto code_point : protocol.code_points()) {
            if (code_point < '\x21' || code_point > '\x7E')
                return WebIDL::SyntaxError::create(realm, "Found invalid character in subprotocol name"_string);
        }
    }

    // 10. Set this's url to urlRecord.
    web_socket->set_url(*url_record);

    // 11. Let client be this’s relevant settings object.
    // 12. Run this step in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(vm.heap(), [web_socket, url_record, protocols_sequence = move(protocols_sequence)]() {
        auto& client = HTML::relevant_settings_object(*web_socket);

        //  1. Establish a WebSocket connection given urlRecord, protocols, and client. [FETCH]
        (void)web_socket->establish_web_socket_connection(*url_record, protocols_sequence, client);
    }));

    return web_socket;
}

WebSocket::WebSocket(JS::Realm& realm)
    : EventTarget(realm)
{
}

WebSocket::~WebSocket() = default;

void WebSocket::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebSocket);
}

ErrorOr<void> WebSocket::establish_web_socket_connection(URL::URL const& url_record, Vector<String> const& protocols, HTML::EnvironmentSettingsObject& client)
{
    // FIXME: Integrate properly with FETCH as per https://fetch.spec.whatwg.org/#websocket-opening-handshake

    auto& window_or_worker = as<HTML::WindowOrWorkerGlobalScopeMixin>(client.global_object());
    auto origin_string = window_or_worker.origin().to_byte_string();

    Vector<ByteString> protcol_byte_strings;
    for (auto const& protocol : protocols)
        TRY(protcol_byte_strings.try_append(protocol.to_byte_string()));

    HTTP::HeaderMap additional_headers;

    auto cookies = ([&] {
        // FIXME: Getting to the page client reliably is way too complicated, and going via the document won't work in workers.
        auto document = client.responsible_document();
        if (!document)
            return String {};

        // NOTE: The WebSocket handshake is sent as an HTTP request, so the source should be Http.
        return document->page().client().page_did_request_cookie(url_record, Cookie::Source::Http);
    })();

    if (!cookies.is_empty()) {
        additional_headers.set("Cookie", cookies.to_byte_string());
    }

    m_websocket = ResourceLoader::the().request_client().websocket_connect(url_record, origin_string, protcol_byte_strings, {}, additional_headers);

    m_websocket->on_open = [weak_this = make_weak_ptr<WebSocket>()] {
        if (!weak_this)
            return;
        auto& websocket = const_cast<WebSocket&>(*weak_this);
        websocket.on_open();
    };
    m_websocket->on_message = [weak_this = make_weak_ptr<WebSocket>()](auto message) {
        if (!weak_this)
            return;
        auto& websocket = const_cast<WebSocket&>(*weak_this);
        websocket.on_message(move(message.data), message.is_text);
    };
    m_websocket->on_close = [weak_this = make_weak_ptr<WebSocket>()](auto code, auto reason, bool was_clean) {
        if (!weak_this)
            return;
        auto& websocket = const_cast<WebSocket&>(*weak_this);
        websocket.on_close(code, String::from_byte_string(reason).release_value_but_fixme_should_propagate_errors(), was_clean);
    };
    m_websocket->on_error = [weak_this = make_weak_ptr<WebSocket>()](auto) {
        if (!weak_this)
            return;
        auto& websocket = const_cast<WebSocket&>(*weak_this);
        websocket.on_error();
    };

    return {};
}

// https://websockets.spec.whatwg.org/#dom-websocket-readystate
Requests::WebSocket::ReadyState WebSocket::ready_state() const
{
    if (m_websocket)
        return m_websocket->ready_state();
    return Requests::WebSocket::ReadyState::Closed;
}

// https://websockets.spec.whatwg.org/#dom-websocket-extensions
String WebSocket::extensions() const
{
    if (!m_websocket)
        return String {};
    // https://websockets.spec.whatwg.org/#feedback-from-the-protocol
    // FIXME: Change the extensions attribute's value to the extensions in use, if it is not the null value.
    return String {};
}

// https://websockets.spec.whatwg.org/#dom-websocket-protocol
WebIDL::ExceptionOr<String> WebSocket::protocol() const
{
    if (!m_websocket)
        return String {};
    return TRY_OR_THROW_OOM(vm(), String::from_byte_string(m_websocket->subprotocol_in_use()));
}

// https://websockets.spec.whatwg.org/#dom-websocket-close
WebIDL::ExceptionOr<void> WebSocket::close(Optional<u16> code, Optional<String> reason)
{
    // 1. If code is present, but is neither an integer equal to 1000 nor an integer in the range 3000 to 4999, inclusive, throw an "InvalidAccessError" DOMException.
    if (code.has_value() && *code != 1000 && (*code < 3000 || *code > 4099))
        return WebIDL::InvalidAccessError::create(realm(), "The close error code is invalid"_string);
    // 2. If reason is present, then run these substeps:
    if (reason.has_value()) {
        // 1. Let reasonBytes be the result of encoding reason.
        // 2. If reasonBytes is longer than 123 bytes, then throw a "SyntaxError" DOMException.
        if (reason->bytes().size() > 123)
            return WebIDL::SyntaxError::create(realm(), "The close reason is longer than 123 bytes"_string);
    }
    // 3. Run the first matching steps from the following list:
    auto state = ready_state();
    // -> If this's ready state is CLOSING (2) or CLOSED (3)
    if (state == Requests::WebSocket::ReadyState::Closing || state == Requests::WebSocket::ReadyState::Closed)
        return {};
    // -> If the WebSocket connection is not yet established [WSP]
    // -> If the WebSocket closing handshake has not yet been started [WSP]
    // -> Otherwise
    // NOTE: All of these are handled by the WebSocket Protocol when calling close()
    // FIXME: LibProtocol does not yet support sending empty Close messages, so we use default values for now
    m_websocket->close(code.value_or(1000), reason.value_or(String {}).to_byte_string());
    return {};
}

// https://websockets.spec.whatwg.org/#dom-websocket-send
WebIDL::ExceptionOr<void> WebSocket::send(Variant<GC::Root<WebIDL::BufferSource>, GC::Root<FileAPI::Blob>, String> const& data)
{
    auto state = ready_state();
    if (state == Requests::WebSocket::ReadyState::Connecting)
        return WebIDL::InvalidStateError::create(realm(), "Websocket is still CONNECTING"_string);
    if (state == Requests::WebSocket::ReadyState::Open) {
        TRY_OR_THROW_OOM(vm(),
            data.visit(
                [this](String const& string) -> ErrorOr<void> {
                    m_websocket->send(string);
                    return {};
                },
                [this](GC::Root<WebIDL::BufferSource> const& buffer_source) -> ErrorOr<void> {
                    // FIXME: While the spec doesn't say to do this, it's not observable except from potentially throwing OOM.
                    //        Can we avoid this copy?
                    auto data_buffer = TRY(WebIDL::get_buffer_source_copy(*buffer_source->raw_object()));
                    m_websocket->send(data_buffer, false);
                    return {};
                },
                [this](GC::Root<FileAPI::Blob> const& blob) -> ErrorOr<void> {
                    auto byte_buffer = TRY(ByteBuffer::copy(blob->raw_bytes()));
                    m_websocket->send(byte_buffer, false);
                    return {};
                }));
        // TODO : If the data cannot be sent, e.g. because it would need to be buffered but the buffer is full, the user agent must flag the WebSocket as full and then close the WebSocket connection.
        // TODO : Any invocation of this method with a string argument that does not throw an exception must increase the bufferedAmount attribute by the number of bytes needed to express the argument as UTF-8.
    }
    return {};
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_open()
{
    // When the WebSocket connection is established, the user agent must queue a task to run these steps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(heap(), [this] {
        // 1. Change the readyState attribute's value to OPEN (1).
        // 2. Change the extensions attribute's value to the extensions in use, if it is not the null value. [WSP]
        // 3. Change the protocol attribute's value to the subprotocol in use, if it is not the null value. [WSP]
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::open));
    }));
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_error()
{
    // When the WebSocket connection is closed, possibly cleanly, the user agent must queue a task to run the following substeps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
    }));
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_close(u16 code, String reason, bool was_clean)
{
    // When the WebSocket connection is closed, possibly cleanly, the user agent must queue a task to run the following substeps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(heap(), [this, code, reason = move(reason), was_clean] {
        // 1. Change the readyState attribute's value to CLOSED. This is handled by the Protocol's WebSocket
        // 2. If [needed], fire an event named error at the WebSocket object. This is handled by the Protocol's WebSocket
        HTML::CloseEventInit event_init {};
        event_init.was_clean = was_clean;
        event_init.code = code;
        event_init.reason = reason;
        dispatch_event(HTML::CloseEvent::create(realm(), HTML::EventNames::close, event_init));
    }));
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_message(ByteBuffer message, bool is_text)
{
    if (m_websocket->ready_state() != Requests::WebSocket::ReadyState::Open)
        return;

    // When a WebSocket message has been received with type type and data data, the user agent must queue a task to follow these steps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(heap(), [this, message = move(message), is_text] {
        if (is_text) {
            auto text_message = ByteString(ReadonlyBytes(message));
            HTML::MessageEventInit event_init;
            event_init.data = JS::PrimitiveString::create(vm(), text_message);
            event_init.origin = url();
            dispatch_event(HTML::MessageEvent::create(realm(), HTML::EventNames::message, event_init));
            return;
        }

        if (m_binary_type == "blob") {
            // type indicates that the data is Binary and binaryType is "blob"
            HTML::MessageEventInit event_init;
            event_init.data = FileAPI::Blob::create(realm(), message, "text/plain;charset=utf-8"_string);
            event_init.origin = url();
            dispatch_event(HTML::MessageEvent::create(realm(), HTML::EventNames::message, event_init));
            return;
        } else if (m_binary_type == "arraybuffer") {
            // type indicates that the data is Binary and binaryType is "arraybuffer"
            HTML::MessageEventInit event_init;
            event_init.data = JS::ArrayBuffer::create(realm(), message);
            event_init.origin = url();
            dispatch_event(HTML::MessageEvent::create(realm(), HTML::EventNames::message, event_init));
            return;
        }

        dbgln("Unsupported WebSocket message type {}", m_binary_type);
        TODO();
    }));
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                       \
    void WebSocket::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                 \
        set_event_handler_attribute(event_name, value);               \
    }                                                                 \
    WebIDL::CallbackType* WebSocket::attribute_name()                 \
    {                                                                 \
        return event_handler_attribute(event_name);                   \
    }
ENUMERATE_WEBSOCKET_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
