/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/JsonObject.h>
#include <LibWeb/DOM/CustomEvent.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/WebUI.h>
#include <LibWeb/WebDriver/JSON.h>
#include <WebContent/WebUIConnection.h>

namespace WebContent {

static auto LADYBIRD_PROPERTY = JS::PropertyKey { "ladybird"_fly_string };
static auto WEB_UI_LOADED_EVENT = "WebUILoaded"_fly_string;
static auto WEB_UI_MESSAGE_EVENT = "WebUIMessage"_fly_string;

ErrorOr<NonnullRefPtr<WebUIConnection>> WebUIConnection::connect(IPC::File web_ui_socket, Web::DOM::Document& document)
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(web_ui_socket.take_fd()));
    TRY(socket->set_blocking(true));

    return adopt_ref(*new WebUIConnection(IPC::Transport { move(socket) }, document));
}

WebUIConnection::WebUIConnection(IPC::Transport transport, Web::DOM::Document& document)
    : IPC::ConnectionFromClient<WebUIClientEndpoint, WebUIServerEndpoint>(*this, move(transport), 1)
    , m_document(document)
{
    auto& realm = m_document->realm();
    m_document->window()->define_direct_property(LADYBIRD_PROPERTY, realm.create<Web::Internals::WebUI>(realm), JS::default_attributes);

    Web::HTML::queue_a_task(Web::HTML::Task::Source::Unspecified, nullptr, m_document, GC::create_function(realm.heap(), [&document = *m_document]() {
        document.dispatch_event(Web::DOM::Event::create(document.realm(), WEB_UI_LOADED_EVENT));
    }));
}

WebUIConnection::~WebUIConnection()
{
    if (!m_document->window())
        return;

    (void)m_document->window()->internal_delete(LADYBIRD_PROPERTY);
}

void WebUIConnection::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
}

void WebUIConnection::send_message(String name, JsonValue data)
{
    if (!m_document->browsing_context())
        return;

    JsonObject detail;
    detail.set("name"sv, move(name));
    detail.set("data"sv, move(data));

    auto& realm = m_document->realm();
    Web::HTML::TemporaryExecutionContext context { realm };

    auto serialized_detail = Web::WebDriver::json_deserialize(*m_document->browsing_context(), detail);
    if (serialized_detail.is_error()) {
        warnln("Unable to serialize JSON data from browser: {}", serialized_detail.error());
        return;
    }

    Web::DOM::CustomEventInit event_init {};
    event_init.detail = serialized_detail.value();

    m_document->dispatch_event(Web::DOM::CustomEvent::create(realm, WEB_UI_MESSAGE_EVENT, event_init));
}

void WebUIConnection::received_message_from_web_ui(String const& name, JS::Value data)
{
    if (!m_document->browsing_context())
        return;

    auto deserialized_data = Web::WebDriver::json_clone(*m_document->browsing_context(), data);
    if (deserialized_data.is_error()) {
        warnln("Unable to deserialize JS data from WebUI: {}", deserialized_data.error());
        return;
    }

    async_received_message(name, deserialized_data.value());
}

}
