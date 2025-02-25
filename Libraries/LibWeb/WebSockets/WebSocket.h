/*
 * Copyright (c) 2021-2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCore/EventReceiver.h>
#include <LibRequests/Forward.h>
#include <LibRequests/WebSocket.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#define ENUMERATE_WEBSOCKET_EVENT_HANDLERS(E) \
    E(onerror, HTML::EventNames::error)       \
    E(onclose, HTML::EventNames::close)       \
    E(onopen, HTML::EventNames::open)         \
    E(onmessage, HTML::EventNames::message)

namespace Web::WebSockets {

class WebSocket final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(WebSocket, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(WebSocket);

public:
    static WebIDL::ExceptionOr<GC::Ref<WebSocket>> construct_impl(JS::Realm&, String const& url, Optional<Variant<String, Vector<String>>> const& protocols);

    virtual ~WebSocket() override;

    String url() const { return m_url.to_string(); }
    void set_url(URL::URL url) { m_url = move(url); }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_WEBSOCKET_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

    Requests::WebSocket::ReadyState ready_state() const;
    String extensions() const;
    WebIDL::ExceptionOr<String> protocol() const;

    String const& binary_type() { return m_binary_type; }
    void set_binary_type(String const& type) { m_binary_type = type; }

    WebIDL::ExceptionOr<void> close(Optional<u16> code, Optional<String> reason);
    WebIDL::ExceptionOr<void> send(Variant<GC::Root<WebIDL::BufferSource>, GC::Root<FileAPI::Blob>, String> const& data);

private:
    void on_open();
    void on_message(ByteBuffer message, bool is_text);
    void on_error();
    void on_close(u16 code, String reason, bool was_clean);

    WebSocket(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual bool must_survive_garbage_collection() const override;

    ErrorOr<void> establish_web_socket_connection(URL::URL const& url_record, Vector<String> const& protocols, HTML::EnvironmentSettingsObject& client);

    URL::URL m_url;
    String m_binary_type { "blob"_string };
    RefPtr<Requests::WebSocket> m_websocket;
};

}
