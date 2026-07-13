/*
 * Copyright (c) 2021-2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Utf16String.h>
#include <LibCore/EventReceiver.h>
#include <LibRequests/Forward.h>
#include <LibRequests/WebSocket.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#define ENUMERATE_WEBSOCKET_EVENT_HANDLERS(E) \
    E(onerror, HTML::EventNames::error)       \
    E(onclose, HTML::EventNames::close)       \
    E(onopen, HTML::EventNames::open)         \
    E(onmessage, HTML::EventNames::message)

namespace Web::WebSockets {

using WebSocketSendData = FlattenVariant<WebIDL::BufferSourceVariant, Variant<GC::Ref<FileAPI::Blob>, Utf16String>>;

class WebSocket final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(WebSocket, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(WebSocket);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;
    static constexpr bool OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION = true;

    static WebIDL::ExceptionOr<GC::Ref<WebSocket>> construct_impl(JS::Realm&, Utf16String const& url, Optional<Variant<Utf16String, Vector<Utf16String>>> const& protocols);

    virtual ~WebSocket() override;

    Utf16String url() const { return Utf16String::from_utf8(m_url.to_string()); }
    void set_url(URL::URL url) { m_url = move(url); }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_WEBSOCKET_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

    Requests::WebSocket::ReadyState ready_state() const;
    Utf16String extensions() const;
    WebIDL::ExceptionOr<Utf16String> protocol() const;

    Utf16String const& binary_type() { return m_binary_type; }
    void set_binary_type(Utf16String const& type) { m_binary_type = type; }

    WebIDL::ExceptionOr<void> close(Optional<u16> code, Optional<Utf16String> reason);
    WebIDL::ExceptionOr<void> send(WebSocketSendData const& data);

    void make_disappear();

private:
    void on_open();
    void on_message(ByteBuffer message, bool is_text);
    void on_error();
    void on_close(u16 code, Utf16String reason, bool was_clean);

    WebSocket(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual bool must_survive_garbage_collection() const override;

    ErrorOr<void> establish_web_socket_connection(URL::URL const& url_record, Vector<Utf16String> const& protocols, HTML::EnvironmentSettingsObject& client);

    URL::URL m_url;
    Utf16String m_binary_type { "blob"_utf16 };
    RefPtr<Requests::WebSocket> m_websocket;

    IntrusiveListNode<WebSocket> m_list_node;

public:
    using List = IntrusiveList<&WebSocket::m_list_node>;
};

}
