/*
 * Copyright (c) 2021-2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
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
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Web::WebSockets {

GC_DEFINE_ALLOCATOR(WebSocket);

// https://websockets.spec.whatwg.org/#dom-websocket-websocket
WebIDL::ExceptionOr<GC::Ref<WebSocket>> WebSocket::create(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, Utf16String const& url, Optional<Variant<Utf16String, Vector<Utf16String>>> const& protocols)
{
    auto web_socket = GC::Heap::the().allocate<WebSocket>(global_scope.this_impl());
    global_scope.register_web_socket({}, web_socket);
    auto& relevant_settings_object = HTML::relevant_settings_object(global_scope);

    // 1. Let baseURL be this's relevant settings object's API base URL.
    auto base_url = relevant_settings_object.api_base_url();

    // 2. Let urlRecord be the result of applying the URL parser to url with baseURL.
    auto url_record = DOMURL::parse(url.utf16_view(), base_url);

    // 3. If urlRecord is failure, then throw a "SyntaxError" DOMException.
    if (!url_record.has_value())
        return WebIDL::SyntaxError::create("Invalid URL"_utf16);

    // 4. If urlRecord’s scheme is "http", then set urlRecord’s scheme to "ws".
    if (url_record->scheme() == "http"sv)
        url_record->set_scheme("ws"_string);
    // 5. Otherwise, if urlRecord’s scheme is "https", set urlRecord’s scheme to "wss".
    else if (url_record->scheme() == "https"sv)
        url_record->set_scheme("wss"_string);

    // 6. If urlRecord’s scheme is not "ws" or "wss", then throw a "SyntaxError" DOMException.
    if (!url_record->scheme().is_one_of("ws"sv, "wss"sv))
        return WebIDL::SyntaxError::create("Invalid protocol"_utf16);

    // 7. If urlRecord’s fragment is non-null, then throw a "SyntaxError" DOMException.
    if (url_record->fragment().has_value())
        return WebIDL::SyntaxError::create("Presence of URL fragment is invalid"_utf16);

    Vector<Utf16String> protocols_sequence;
    // 8. If protocols is a string, set protocols to a sequence consisting of just that string.
    if (protocols.has_value() && protocols->has<Utf16String>())
        protocols_sequence = { protocols.value().get<Utf16String>() };
    else if (protocols.has_value() && protocols->has<Vector<Utf16String>>())
        protocols_sequence = protocols.value().get<Vector<Utf16String>>();
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
        if (protocol.is_empty())
            return WebIDL::SyntaxError::create("Found empty protocol name"_utf16);
        if (i < sorted_protocols.size() - 1 && protocol == sorted_protocols[i + 1])
            return WebIDL::SyntaxError::create("Found a duplicate protocol name in the specified list"_utf16);
        for (auto code_point : protocol.utf16_view()) {
            if (code_point < '\x21' || code_point > '\x7E')
                return WebIDL::SyntaxError::create("Found invalid character in subprotocol name"_utf16);
        }
    }

    // 10. Set this's url to urlRecord.
    web_socket->set_url(*url_record);

    // 11. Let client be this’s relevant settings object.
    // 12. Run this step in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [web_socket, url_record, protocols_sequence = move(protocols_sequence), client = GC::Ref { relevant_settings_object }]() {
        // 1. Establish a WebSocket connection given urlRecord, protocols, and client. [FETCH]
        // AD-HOC: We don't yet implement this method to spec, so it's possible for the connection to fail before we
        //         make a Requests::WebSocket. If so, we need to manually error and close it.
        if (web_socket->establish_web_socket_connection(*url_record, protocols_sequence, *client).is_error()) {
            web_socket->on_error();
            web_socket->on_close(to_underlying(::WebSocket::CloseStatusCode::AbnormalClosure), Utf16String {}, false);
        }
    }));

    return web_socket;
}

WebIDL::ExceptionOr<GC::Ref<WebSocket>> WebSocket::create_for_constructor(JS::Object& relevant_global_object, Utf16String const& url, Optional<Variant<Utf16String, Vector<Utf16String>>> const& protocols)
{
    auto& global_scope = HTML::relevant_window_or_worker_global_scope(relevant_global_object);
    return create(global_scope, url, protocols);
}

WebSocket::WebSocket(GC::Ref<DOM::EventTarget> relevant_global_object)
    : EventTarget()
    , m_global_object(relevant_global_object)
{
}

WebSocket::~WebSocket() = default;

JS::Object& WebSocket::relevant_global_object() const
{
    return HTML::relevant_global_object(relevant_global_scope());
}

void WebSocket::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_object);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#garbage-collection
void WebSocket::finalize()
{
    Base::finalize();

    auto ready_state = this->ready_state();

    // If a WebSocket object is garbage collected while its connection is still open, the user agent must start the
    // WebSocket closing handshake, with no status code for the Close message. [WSP]
    if (ready_state != Requests::WebSocket::ReadyState::Closing && ready_state != Requests::WebSocket::ReadyState::Closed) {
        // FIXME: LibProtocol does not yet support sending empty Close messages, so we use default values for now
        m_websocket->close(1000);
    }

    m_activity_root.release();
    relevant_global_scope().unregister_web_socket({}, *this);
}

HTML::WindowOrWorkerGlobalScopeMixin& WebSocket::relevant_global_scope() const
{
    return HTML::relevant_window_or_worker_global_scope(*m_global_object);
}

ErrorOr<void> WebSocket::establish_web_socket_connection(URL::URL const& url_record, Vector<Utf16String> const& protocols, HTML::EnvironmentSettingsObject& client)
{
    // FIXME: Integrate properly with FETCH as per https://fetch.spec.whatwg.org/#websocket-opening-handshake
    //        That means following https://websockets.spec.whatwg.org/#concept-websocket-establish

    auto* window_or_worker = HTML::window_or_worker_global_scope_from_global_object(client.global_object());
    VERIFY(window_or_worker);
    auto origin_string = window_or_worker->origin().to_byte_string();

    Vector<ByteString> protocol_byte_strings;
    for (auto const& protocol : protocols)
        TRY(protocol_byte_strings.try_append(protocol.to_utf8().to_byte_string()));

    auto additional_headers = HTTP::HeaderList::create();

    auto cookies = ([&] {
        auto& page = Bindings::principal_host_defined_page(HTML::relevant_realm(relevant_global_object()));
        return page.client().page_did_request_cookie(url_record, HTTP::Cookie::Source::Http).cookie;
    })();

    if (!cookies.is_empty()) {
        additional_headers->append({ "Cookie"sv, cookies.to_byte_string() });
    }

    additional_headers->append({ "User-Agent"sv, ResourceLoader::the().user_agent_for_websocket_url(url_record).to_byte_string() });

    auto request_client = ResourceLoader::the().request_client();

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (!request_client)
        return Error::from_string_literal("RequestServer is currently unavailable");

    m_websocket = request_client->websocket_connect(url_record, origin_string, protocol_byte_strings, {}, additional_headers);

    m_websocket->on_open = GC::weak_callback(*this, [](auto& self) {
        self.on_open();
    });
    m_websocket->on_message = GC::weak_callback(*this, [](auto& self, auto message) {
        self.on_message(move(message.data), message.is_text);
    });
    m_websocket->on_close = GC::weak_callback(*this, [](auto& self, auto code, auto reason, bool was_clean) {
        self.on_close(code, Utf16String::from_utf8(StringView { reason.bytes() }), was_clean);
    });
    m_websocket->on_error = GC::weak_callback(*this, [](auto& self, auto) {
        self.on_error();
    });
    m_websocket->on_ready_state_change = GC::weak_callback(*this, [](auto& self) {
        self.update_activity_root();
    });

    update_activity_root();
    return {};
}

bool WebSocket::should_be_kept_alive() const
{
    if (m_has_disappeared)
        return false;

    auto state = ready_state();
    if (state == Requests::WebSocket::ReadyState::Connecting) {
        return has_event_listener(HTML::EventNames::open)
            || has_event_listener(HTML::EventNames::message)
            || has_event_listener(HTML::EventNames::error)
            || has_event_listener(HTML::EventNames::close);
    }
    if (state == Requests::WebSocket::ReadyState::Open) {
        return has_event_listener(HTML::EventNames::message)
            || has_event_listener(HTML::EventNames::error)
            || has_event_listener(HTML::EventNames::close);
    }
    if (state == Requests::WebSocket::ReadyState::Closing) {
        return has_event_listener(HTML::EventNames::error)
            || has_event_listener(HTML::EventNames::close);
    }
    return false;
}

void WebSocket::update_activity_root()
{
    if (should_be_kept_alive())
        m_activity_root.take(*this);
    else
        m_activity_root.release();
}

void WebSocket::event_listener_list_changed()
{
    update_activity_root();
}

// https://websockets.spec.whatwg.org/#dom-websocket-readystate
Requests::WebSocket::ReadyState WebSocket::ready_state() const
{
    if (m_websocket)
        return m_websocket->ready_state();
    return Requests::WebSocket::ReadyState::Closed;
}

// https://websockets.spec.whatwg.org/#dom-websocket-extensions
Utf16String WebSocket::extensions() const
{
    if (!m_websocket)
        return {};
    // https://websockets.spec.whatwg.org/#feedback-from-the-protocol
    // FIXME: Change the extensions attribute's value to the extensions in use, if it is not the null value.
    return {};
}

// https://websockets.spec.whatwg.org/#dom-websocket-protocol
WebIDL::ExceptionOr<Utf16String> WebSocket::protocol() const
{
    if (!m_websocket)
        return Utf16String {};
    auto subprotocol = m_websocket->subprotocol_in_use();
    return Utf16String::from_utf8(StringView { subprotocol.bytes() });
}

// https://websockets.spec.whatwg.org/#dom-websocket-close
WebIDL::ExceptionOr<void> WebSocket::close(Optional<u16> code, Optional<Utf16String> reason)
{
    // 1. If code is present, but is neither an integer equal to 1000 nor an integer in the range 3000 to 4999, inclusive, throw an "InvalidAccessError" DOMException.
    if (code.has_value() && *code != 1000 && (*code < 3000 || *code > 4999))
        return WebIDL::InvalidAccessError::create("The close error code is invalid"_utf16);
    // 2. If reason is present, then run these substeps:
    String encoded_reason;
    if (reason.has_value()) {
        // 1. Let reasonBytes be the result of encoding reason.
        encoded_reason = reason->to_utf8();

        // 2. If reasonBytes is longer than 123 bytes, then throw a "SyntaxError" DOMException.
        if (encoded_reason.bytes().size() > 123)
            return WebIDL::SyntaxError::create("The close reason is longer than 123 bytes"_utf16);
    }
    // 3. Run the first matching steps from the following list:
    auto state = ready_state();
    // -> If this's ready state is CLOSING (2) or CLOSED (3)
    if (state == Requests::WebSocket::ReadyState::Closing || state == Requests::WebSocket::ReadyState::Closed)
        return {};
    // -> If the WebSocket connection is not yet established [WSP]
    // -> If the WebSocket closing handshake has not yet been started [WSP]
    // -> Otherwise
    // NB: All of these are handled by the WebSocket Protocol when calling close(). We still set the ready state to
    //     CLOSING now though (which every case above expects), to prevent handling any messages from the remote server
    //     in the meantime.
    m_websocket->set_ready_state(Requests::WebSocket::ReadyState::Closing);
    update_activity_root();

    // FIXME: LibProtocol does not yet support sending empty Close messages, so we use default values for now
    m_websocket->close(code.value_or(1000), encoded_reason.to_byte_string());
    return {};
}

// https://websockets.spec.whatwg.org/#dom-websocket-send
WebIDL::ExceptionOr<void> WebSocket::send(WebSocketSendData const& data)
{
    auto state = ready_state();
    if (state == Requests::WebSocket::ReadyState::Connecting)
        return WebIDL::InvalidStateError::create("Websocket is still CONNECTING"_utf16);
    if (state == Requests::WebSocket::ReadyState::Open) {
        ByteBuffer buffer_storage;
        data.visit(
            [this](Utf16String const& string) {
                auto encoded_string = string.to_utf8();
                m_websocket->send(encoded_string);
            },
            [this, &buffer_storage](auto const& buffer_source_value) {
                WebIDL::BufferSource buffer_source { WebIDL::BufferSourceVariant { buffer_source_value } };
                ReadonlyBytes buffer;

                if (auto array_buffer = buffer_source.viewed_array_buffer(); array_buffer && !array_buffer->is_detached() && !buffer_source.is_out_of_bounds()) {
                    buffer_storage = MUST(ByteBuffer::create_uninitialized(buffer_source.byte_length()));
                    array_buffer->copy_to(buffer_source.byte_offset(), buffer_storage);
                    buffer = buffer_storage;
                }

                m_websocket->send(buffer, false);
            },
            [this](GC::Ref<FileAPI::Blob> blob) {
                m_websocket->send(blob->raw_bytes(), false);
            });
        // TODO : If the data cannot be sent, e.g. because it would need to be buffered but the buffer is full, the user agent must flag the WebSocket as full and then close the WebSocket connection.
        // TODO : Any invocation of this method with a string argument that does not throw an exception must increase the bufferedAmount attribute by the number of bytes needed to express the argument as UTF-8.
    }
    return {};
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_open()
{
    update_activity_root();

    // When the WebSocket connection is established, the user agent must queue a task to run these steps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this] {
        // 1. Change the readyState attribute's value to OPEN (1).
        // 2. Change the extensions attribute's value to the extensions in use, if it is not the null value. [WSP]
        // 3. Change the protocol attribute's value to the subprotocol in use, if it is not the null value. [WSP]
        dispatch_event(DOM::Event::create(HTML::EventNames::open, HighResolutionTime::current_high_resolution_time(relevant_global_object())));
    }));
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_message(ByteBuffer message, bool is_text)
{
    // When a WebSocket message has been received with type type and data data, the user agent must queue a task to follow these steps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this, message = move(message), is_text] mutable {
        // 1. If ready state is not OPEN (1), then return.
        if (m_websocket->ready_state() != Requests::WebSocket::ReadyState::Open)
            return;

        auto& realm = HTML::relevant_realm(relevant_global_object());

        // 2. Let dataForEvent be determined by switching on type and binary type:
        auto data_for_event = [&]() -> JS::Value {
            // -> type indicates that the data is Text
            if (is_text) {
                // a new DOMString containing data
                return JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(message));
            }
            // -> type indicates that the data is Binary and binary type is "blob"
            if (m_binary_type == Bindings::BinaryType::Blob) {
                // a new Blob object, created in the relevant Realm of the WebSocket object, that represents data as its raw data [FILEAPI]
                return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, FileAPI::Blob::create(move(message), Utf16String {}));
            }
            // -> type indicates that the data is Binary and binary type is "arraybuffer"
            if (m_binary_type == Bindings::BinaryType::Arraybuffer) {
                // a new ArrayBuffer object, created in the relevant Realm of the WebSocket object, whose contents are data
                return JS::ArrayBuffer::create(realm, move(message));
            }

            VERIFY_NOT_REACHED();
        }();

        // 3. Fire an event named message at the WebSocket object, using MessageEvent, with the origin attribute
        //    initialized to the serialization of the WebSocket object’s url’s origin, and the data attribute
        //    initialized to dataForEvent.
        HTML::MessageEventInit event_init;
        event_init.data = data_for_event;

        dispatch_event(HTML::MessageEvent::create(realm.global_object(), HTML::EventNames::message, event_init, m_url.origin()));
    }));
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_error()
{
    // When the WebSocket connection is closed, possibly cleanly, the user agent must queue a task to run the following substeps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this] {
        dispatch_event(DOM::Event::create(HTML::EventNames::error, HighResolutionTime::current_high_resolution_time(relevant_global_object())));
    }));
}

// https://websockets.spec.whatwg.org/#feedback-from-the-protocol
void WebSocket::on_close(u16 code, Utf16String reason, bool was_clean)
{
    update_activity_root();

    // When the WebSocket connection is closed, possibly cleanly, the user agent must queue a task to run the following substeps:
    HTML::queue_a_task(HTML::Task::Source::WebSocket, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this, code, reason = move(reason), was_clean] {
        // 1. Change the readyState attribute's value to CLOSED. This is handled by the Protocol's WebSocket
        // 2. If [needed], fire an event named error at the WebSocket object. This is handled by the Protocol's WebSocket
        HTML::CloseEventInit event_init {};
        event_init.was_clean = was_clean;
        event_init.code = code;
        event_init.reason = reason;
        dispatch_event(HTML::CloseEvent::create(HTML::EventNames::close, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object())));
    }));
}

// https://websockets.spec.whatwg.org/#make-disappear
void WebSocket::make_disappear()
{
    m_has_disappeared = true;

    // -> If the WebSocket connection is not yet established [WSP]
    //    - Fail the WebSocket connection. [WSP]
    // -> If the WebSocket closing handshake has not yet been started [WSP]
    //    - Start the WebSocket closing handshake, with the status code to use in the WebSocket Close message being 1001. [WSP]
    // -> Otherwise
    //    - Do nothing.
    // NOTE: All of these are handled by the WebSocket Protocol when calling close()
    auto ready_state = this->ready_state();
    if (ready_state == Requests::WebSocket::ReadyState::Closing || ready_state == Requests::WebSocket::ReadyState::Closed) {
        m_activity_root.release();
        return;
    }

    m_websocket->close(1001);
    m_activity_root.release();
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
