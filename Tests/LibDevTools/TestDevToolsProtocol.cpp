/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibHTTP/Header.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWebView/Attribute.h>
#include <LibWebView/ConsoleOutput.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Mutation.h>

using namespace AK::TimeLiterals;

static void pump(Core::EventLoop& loop)
{
    (void)loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}

static void spin_until(Core::EventLoop& loop, Function<bool()> condition, AK::Duration timeout = 2000_ms)
{
    for (i64 elapsed_ms = 0; elapsed_ms < timeout.to_milliseconds(); elapsed_ms += 5) {
        pump(loop);
        if (condition())
            return;
        MUST(Core::System::sleep_ms(5));
    }
    FAIL("Timed out waiting for condition");
}

static JsonObject make_node(Web::UniqueNodeID id, StringView type, StringView name)
{
    JsonObject node;
    node.set("id"sv, id.value());
    node.set("type"sv, type);
    node.set("name"sv, name);
    return node;
}

static JsonObject make_dom_tree()
{
    JsonObject target = make_node(4, "element"sv, "DIV"sv);
    target.set("visible"sv, true);
    target.set("scrollable"sv, true);

    JsonObject target_attributes;
    target_attributes.set("id"sv, "target"sv);
    target_attributes.set("class"sv, "fixture"sv);
    target.set("attributes"sv, move(target_attributes));

    JsonObject text = make_node(5, "text"sv, "#text"sv);
    text.set("text"sv, "Hello"sv);

    JsonObject comment = make_node(6, "comment"sv, "#comment"sv);
    comment.set("data"sv, "A comment"sv);

    JsonObject whitespace = make_node(7, "text"sv, "#text"sv);
    whitespace.set("text"sv, "   \n  "sv);

    JsonObject before = make_node(40, "pseudo-element"sv, "::before"sv);
    before.set("parent-id"sv, 4);
    before.set("pseudo-element"sv, to_underlying(Web::CSS::PseudoElement::Before));

    JsonArray target_children;
    target_children.must_append(move(text));
    target_children.must_append(move(comment));
    target_children.must_append(move(whitespace));
    target_children.must_append(move(before));
    target.set("children"sv, move(target_children));

    JsonObject sibling = make_node(8, "element"sv, "SPAN"sv);
    sibling.set("visible"sv, true);

    JsonArray body_children;
    body_children.must_append(move(sibling));
    body_children.must_append(move(target));

    JsonObject body = make_node(3, "element"sv, "BODY"sv);
    body.set("visible"sv, true);
    body.set("children"sv, move(body_children));

    JsonArray html_children;
    html_children.must_append(move(body));

    JsonObject html = make_node(2, "element"sv, "HTML"sv);
    html.set("visible"sv, true);
    html.set("children"sv, move(html_children));

    JsonArray document_children;
    document_children.must_append(move(html));

    JsonObject document = make_node(1, "document"sv, "#document"sv);
    document.set("children"sv, move(document_children));
    return document;
}

static JsonObject make_accessibility_tree()
{
    JsonObject button = make_node(4, "element"sv, "Target button"sv);
    button.set("role"sv, "pushbutton"sv);

    JsonArray children;
    children.must_append(move(button));

    JsonObject root = make_node(1, "document"sv, "Document"sv);
    root.set("role"sv, "document"sv);
    root.set("children"sv, move(children));
    return root;
}

static WebView::DOMNodeProperties make_computed_style()
{
    JsonObject properties;
    properties.set("display"sv, "block"sv);
    properties.set("color"sv, "rgb(1, 2, 3)"sv);
    return { WebView::DOMNodeProperties::Type::ComputedStyle, move(properties) };
}

static WebView::DOMNodeProperties make_layout()
{
    JsonObject properties;
    properties.set("width"sv, "100px"sv);
    properties.set("height"sv, "50px"sv);
    properties.set("border-top-width"sv, 1);
    properties.set("border-right-width"sv, 2);
    properties.set("border-bottom-width"sv, 3);
    properties.set("border-left-width"sv, 4);
    properties.set("margin-top"sv, 5);
    properties.set("margin-right"sv, 6);
    properties.set("margin-bottom"sv, 7);
    properties.set("margin-left"sv, 8);
    properties.set("padding-top"sv, 9);
    properties.set("padding-right"sv, 10);
    properties.set("padding-bottom"sv, 11);
    properties.set("padding-left"sv, 12);
    properties.set("box-sizing"sv, "border-box"sv);
    properties.set("display"sv, "block"sv);
    properties.set("float"sv, "none"sv);
    properties.set("line-height"sv, "normal"sv);
    properties.set("position"sv, "static"sv);
    properties.set("z-index"sv, "auto"sv);
    return { WebView::DOMNodeProperties::Type::Layout, move(properties) };
}

static WebView::DOMNodeProperties make_used_fonts()
{
    JsonArray fonts;
    JsonObject font;
    font.set("name"sv, "Test Sans"sv);
    font.set("weight"sv, 400);
    fonts.must_append(move(font));
    return { WebView::DOMNodeProperties::Type::UsedFonts, move(fonts) };
}

static Web::CSS::StyleSheetIdentifier fixture_style_sheet()
{
    return { .type = Web::CSS::StyleSheetIdentifier::Type::StyleElement,
        .dom_element_unique_id = 9,
        .url = "https://example.test/style.css"_string,
        .rule_count = 2 };
}

class TestDevToolsDelegate final : public DevTools::DevToolsDelegate {
public:
    virtual Vector<DevTools::TabDescription> tab_list() const override
    {
        return { { .id = 1, .title = "Fixture page"_string, .url = "https://example.test/"_string } };
    }

    virtual Vector<DevTools::CSSProperty> css_property_list() const override
    {
        Vector<DevTools::CSSProperty> properties;
        properties.append({ "display"_string, false });
        properties.append({ "color"_string, true });
        return properties;
    }

    virtual void inspect_tab(DevTools::TabDescription const&, OnTabInspectionComplete callback) const override
    {
        ++inspect_tab_call_count;
        callback(make_dom_tree());
    }

    virtual void inspect_accessibility_tree(DevTools::TabDescription const&, OnAccessibilityTreeInspectionComplete callback) const override
    {
        ++inspect_accessibility_tree_call_count;
        callback(make_accessibility_tree());
    }

    virtual void listen_for_dom_properties(DevTools::TabDescription const&, OnDOMNodePropertiesReceived callback) const override
    {
        ++listen_for_dom_properties_call_count;
        on_dom_node_properties = move(callback);
    }

    virtual void stop_listening_for_dom_properties(DevTools::TabDescription const&) const override
    {
        ++stop_listening_for_dom_properties_call_count;
    }

    virtual void inspect_dom_node(DevTools::TabDescription const&, WebView::DOMNodeProperties::Type type, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) const override
    {
        ++inspect_dom_node_call_count;
        last_inspected_dom_node = node_id;
        last_inspected_pseudo_element = pseudo_element;

        Core::deferred_invoke([this, type] {
            VERIFY(on_dom_node_properties);
            if (type == WebView::DOMNodeProperties::Type::ComputedStyle)
                on_dom_node_properties(make_computed_style());
            else if (type == WebView::DOMNodeProperties::Type::Layout)
                on_dom_node_properties(make_layout());
            else
                on_dom_node_properties(make_used_fonts());
        });
    }

    virtual void clear_inspected_dom_node(DevTools::TabDescription const&) const override { ++clear_inspected_dom_node_call_count; }
    virtual void clear_highlighted_dom_node(DevTools::TabDescription const&) const override { ++clear_highlighted_dom_node_call_count; }

    virtual void highlight_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) const override
    {
        ++highlight_dom_node_call_count;
        last_highlighted_dom_node = node_id;
        last_highlighted_pseudo_element = pseudo_element;
    }

    virtual void listen_for_dom_mutations(DevTools::TabDescription const&, OnDOMMutationReceived callback) const override
    {
        ++listen_for_dom_mutations_call_count;
        on_dom_mutation = move(callback);
    }

    virtual void stop_listening_for_dom_mutations(DevTools::TabDescription const&) const override { ++stop_listening_for_dom_mutations_call_count; }

    virtual void get_dom_node_inner_html(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeHTMLReceived callback) const override
    {
        ++get_dom_node_inner_html_call_count;
        last_edited_node = node_id;
        callback("<span>inner</span>"_string);
    }

    virtual void get_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeHTMLReceived callback) const override
    {
        ++get_dom_node_outer_html_call_count;
        last_edited_node = node_id;
        callback("<div id=\"target\"></div>"_string);
    }

    virtual void set_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& html, OnDOMNodeEditComplete callback) const override
    {
        ++set_dom_node_outer_html_call_count;
        last_edited_node = node_id;
        last_html = html;
        callback(node_id);
    }

    virtual void set_dom_node_text(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& text, OnDOMNodeEditComplete callback) const override
    {
        ++set_dom_node_text_call_count;
        last_edited_node = node_id;
        last_text = text;
        callback(node_id);
    }

    virtual void set_dom_node_tag(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& tag_name, OnDOMNodeEditComplete callback) const override
    {
        ++set_dom_node_tag_call_count;
        last_edited_node = node_id;
        last_tag = tag_name;
        callback(node_id);
    }

    virtual void add_dom_node_attributes(DevTools::TabDescription const&, Web::UniqueNodeID node_id, ReadonlySpan<WebView::Attribute> attributes, OnDOMNodeEditComplete callback) const override
    {
        ++add_dom_node_attributes_call_count;
        last_edited_node = node_id;
        last_attribute_count = attributes.size();
        callback(node_id);
    }

    virtual void replace_dom_node_attribute(DevTools::TabDescription const&, Web::UniqueNodeID node_id, String const& attribute, ReadonlySpan<WebView::Attribute> attributes, OnDOMNodeEditComplete callback) const override
    {
        ++replace_dom_node_attribute_call_count;
        last_edited_node = node_id;
        last_attribute = attribute;
        last_attribute_count = attributes.size();
        callback(node_id);
    }

    virtual void create_child_element(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeEditComplete callback) const override
    {
        ++create_child_element_call_count;
        last_edited_node = node_id;
        callback(node_id);
    }

    virtual void insert_dom_node_before(DevTools::TabDescription const&, Web::UniqueNodeID node_id, Web::UniqueNodeID parent_id, Optional<Web::UniqueNodeID> sibling_id, OnDOMNodeEditComplete callback) const override
    {
        ++insert_dom_node_before_call_count;
        last_edited_node = node_id;
        last_parent_node = parent_id;
        last_sibling_node = sibling_id;
        callback(node_id);
    }

    virtual void clone_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeEditComplete callback) const override
    {
        ++clone_dom_node_call_count;
        last_edited_node = node_id;
        callback(node_id);
    }

    virtual void remove_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID node_id, OnDOMNodeEditComplete callback) const override
    {
        ++remove_dom_node_call_count;
        last_edited_node = node_id;
        callback(node_id);
    }

    virtual void retrieve_style_sheets(DevTools::TabDescription const&, OnStyleSheetsReceived callback) const override
    {
        ++retrieve_style_sheets_call_count;
        Vector<Web::CSS::StyleSheetIdentifier> style_sheets;
        style_sheets.append(fixture_style_sheet());
        callback(move(style_sheets));
    }

    virtual void retrieve_style_sheet_source(DevTools::TabDescription const&, Web::CSS::StyleSheetIdentifier const&) const override
    {
        ++retrieve_style_sheet_source_call_count;
        Core::deferred_invoke([this] {
            VERIFY(on_style_sheet_source);
            on_style_sheet_source(fixture_style_sheet(), "body { color: red; }"_string);
        });
    }

    virtual void listen_for_style_sheet_sources(DevTools::TabDescription const&, OnStyleSheetSourceReceived callback) const override
    {
        ++listen_for_style_sheet_sources_call_count;
        on_style_sheet_source = move(callback);
    }

    virtual void stop_listening_for_style_sheet_sources(DevTools::TabDescription const&) const override { ++stop_listening_for_style_sheet_sources_call_count; }

    virtual void listen_for_console_messages(DevTools::TabDescription const&, OnConsoleMessage callback) const override
    {
        ++listen_for_console_messages_call_count;
        on_console_message = move(callback);
    }

    virtual void stop_listening_for_console_messages(DevTools::TabDescription const&) const override { ++stop_listening_for_console_messages_call_count; }

    virtual void listen_for_network_events(DevTools::TabDescription const&, OnNetworkRequestStarted on_request_started, OnNetworkResponseHeadersReceived on_headers_received, OnNetworkResponseBodyReceived on_body_received, OnNetworkRequestFinished on_request_finished) const override
    {
        ++listen_for_network_events_call_count;
        on_network_request_started = move(on_request_started);
        on_network_response_headers_received = move(on_headers_received);
        on_network_response_body_received = move(on_body_received);
        on_network_request_finished = move(on_request_finished);
    }

    virtual void stop_listening_for_network_events(DevTools::TabDescription const&) const override { ++stop_listening_for_network_events_call_count; }

    virtual void listen_for_navigation_events(DevTools::TabDescription const&, OnNavigationStarted on_started, OnNavigationFinished on_finished) const override
    {
        ++listen_for_navigation_events_call_count;
        on_navigation_started = move(on_started);
        on_navigation_finished = move(on_finished);
    }

    virtual void stop_listening_for_navigation_events(DevTools::TabDescription const&) const override { ++stop_listening_for_navigation_events_call_count; }
    virtual void did_connect_devtools_client(DevTools::TabDescription const&) const override { ++did_connect_devtools_client_call_count; }
    virtual void did_disconnect_devtools_client(DevTools::TabDescription const&) const override { ++did_disconnect_devtools_client_call_count; }

    void emit_mutation(WebView::Mutation mutation) const
    {
        VERIFY(on_dom_mutation);
        on_dom_mutation(move(mutation));
    }

    void emit_console_log(JS::Console::LogLevel level, Vector<JsonValue> arguments) const
    {
        VERIFY(on_console_message);
        on_console_message({ UnixDateTime::from_seconds_since_epoch(10), WebView::ConsoleLog { level, move(arguments) } });
    }

    void emit_console_trace() const
    {
        VERIFY(on_console_message);
        Vector<WebView::StackFrame> stack;
        stack.append({ "traceFunction"_string, "trace.js"_string, 12, 34 });
        on_console_message({ UnixDateTime::from_seconds_since_epoch(11), WebView::ConsoleTrace { "trace label"_string, move(stack) } });
    }

    void emit_console_error() const
    {
        VERIFY(on_console_message);
        Vector<WebView::StackFrame> stack;
        stack.append({ "boom"_string, "error.js"_string, 56, 78 });
        on_console_message({ UnixDateTime::from_seconds_since_epoch(12), WebView::ConsoleError { "TypeError"_string, "bad things"_string, move(stack), true } });
    }

    void emit_network_lifecycle() const
    {
        VERIFY(on_network_request_started);
        VERIFY(on_network_response_headers_received);
        VERIFY(on_network_response_body_received);
        VERIFY(on_network_request_finished);

        ByteBuffer request_body;
        request_body.append("request", 7);

        Vector<HTTP::Header> request_headers;
        request_headers.append({ ByteString::formatted("Accept"), ByteString::formatted("*/*") });

        on_network_request_started({ .request_id = 100,
            .url = "https://example.test/data.json"_string,
            .method = "POST"_string,
            .start_time = UnixDateTime::from_seconds_since_epoch(20),
            .request_headers = move(request_headers),
            .request_body = move(request_body),
            .initiator_type = "fetch"_string });

        Vector<HTTP::Header> response_headers;
        response_headers.append({ ByteString::formatted("Content-Type"), ByteString::formatted("application/json") });
        on_network_response_headers_received({ .request_id = 100,
            .status_code = 200,
            .reason_phrase = "OK"_string,
            .response_headers = move(response_headers) });

        ByteBuffer response_body;
        response_body.append("{\"ok\":true}", 11);
        on_network_response_body_received(100, move(response_body));

        Requests::RequestTimingInfo timing_info;
        timing_info.request_start_microseconds = 1000;
        timing_info.domain_lookup_start_microseconds = 1000;
        timing_info.domain_lookup_end_microseconds = 2000;
        timing_info.connect_start_microseconds = 2000;
        timing_info.connect_end_microseconds = 4000;
        timing_info.response_start_microseconds = 5000;
        timing_info.response_end_microseconds = 9000;
        on_network_request_finished({ .request_id = 100, .body_size = 11, .timing_info = timing_info, .network_error = {} });
    }

    void emit_navigation() const
    {
        VERIFY(on_navigation_started);
        VERIFY(on_navigation_finished);
        on_navigation_started("https://example.test/next"_string);
        on_navigation_finished("https://example.test/next"_string, "Next page"_string);
    }

    mutable Function<void(WebView::DOMNodeProperties)> on_dom_node_properties;
    mutable Function<void(WebView::Mutation)> on_dom_mutation;
    mutable Function<void(Web::CSS::StyleSheetIdentifier const&, String)> on_style_sheet_source;
    mutable Function<void(WebView::ConsoleOutput)> on_console_message;
    mutable Function<void(DevToolsDelegate::NetworkRequestData)> on_network_request_started;
    mutable Function<void(DevToolsDelegate::NetworkResponseData)> on_network_response_headers_received;
    mutable Function<void(u64, ByteBuffer)> on_network_response_body_received;
    mutable Function<void(DevToolsDelegate::NetworkRequestCompleteData)> on_network_request_finished;
    mutable Function<void(String)> on_navigation_started;
    mutable Function<void(String, String)> on_navigation_finished;

    mutable size_t inspect_tab_call_count { 0 };
    mutable size_t inspect_accessibility_tree_call_count { 0 };
    mutable size_t listen_for_dom_properties_call_count { 0 };
    mutable size_t stop_listening_for_dom_properties_call_count { 0 };
    mutable size_t inspect_dom_node_call_count { 0 };
    mutable size_t clear_inspected_dom_node_call_count { 0 };
    mutable size_t highlight_dom_node_call_count { 0 };
    mutable size_t clear_highlighted_dom_node_call_count { 0 };
    mutable size_t listen_for_dom_mutations_call_count { 0 };
    mutable size_t stop_listening_for_dom_mutations_call_count { 0 };
    mutable size_t get_dom_node_inner_html_call_count { 0 };
    mutable size_t get_dom_node_outer_html_call_count { 0 };
    mutable size_t set_dom_node_outer_html_call_count { 0 };
    mutable size_t set_dom_node_text_call_count { 0 };
    mutable size_t set_dom_node_tag_call_count { 0 };
    mutable size_t add_dom_node_attributes_call_count { 0 };
    mutable size_t replace_dom_node_attribute_call_count { 0 };
    mutable size_t create_child_element_call_count { 0 };
    mutable size_t insert_dom_node_before_call_count { 0 };
    mutable size_t clone_dom_node_call_count { 0 };
    mutable size_t remove_dom_node_call_count { 0 };
    mutable size_t retrieve_style_sheets_call_count { 0 };
    mutable size_t retrieve_style_sheet_source_call_count { 0 };
    mutable size_t listen_for_style_sheet_sources_call_count { 0 };
    mutable size_t stop_listening_for_style_sheet_sources_call_count { 0 };
    mutable size_t listen_for_console_messages_call_count { 0 };
    mutable size_t stop_listening_for_console_messages_call_count { 0 };
    mutable size_t listen_for_network_events_call_count { 0 };
    mutable size_t stop_listening_for_network_events_call_count { 0 };
    mutable size_t listen_for_navigation_events_call_count { 0 };
    mutable size_t stop_listening_for_navigation_events_call_count { 0 };
    mutable size_t did_connect_devtools_client_call_count { 0 };
    mutable size_t did_disconnect_devtools_client_call_count { 0 };

    mutable Optional<Web::UniqueNodeID> last_highlighted_dom_node;
    mutable Optional<Web::CSS::PseudoElement> last_highlighted_pseudo_element;
    mutable Optional<Web::UniqueNodeID> last_inspected_dom_node;
    mutable Optional<Web::CSS::PseudoElement> last_inspected_pseudo_element;
    mutable Optional<Web::UniqueNodeID> last_edited_node;
    mutable Optional<Web::UniqueNodeID> last_parent_node;
    mutable Optional<Web::UniqueNodeID> last_sibling_node;
    mutable Optional<String> last_html;
    mutable Optional<String> last_text;
    mutable Optional<String> last_tag;
    mutable Optional<String> last_attribute;
    mutable size_t last_attribute_count { 0 };
};

class ProtocolClient {
public:
    static NonnullOwnPtr<ProtocolClient> connect(Core::EventLoop& loop, DevTools::DevToolsServer& server)
    {
        auto port = server.local_port();
        VERIFY(port.has_value());
        auto socket = MUST(Core::TCPSocket::connect("127.0.0.1", *port));
        MUST(socket->set_blocking(false));

        auto client = adopt_own(*new ProtocolClient(loop, move(socket)));
        spin_until(loop, [&] { return !!server.connection(); });
        return client;
    }

    JsonObject read_message()
    {
        if (!m_pending_messages.is_empty()) {
            auto message = move(m_pending_messages.first());
            m_pending_messages.remove(0);
            return message;
        }
        return read_message_from_socket();
    }

    void send(JsonObject message)
    {
        auto serialized = message.serialized();
        auto packet = MUST(String::formatted("{}:{}", serialized.byte_count(), serialized));
        MUST(m_socket->set_blocking(true));
        auto restore_nonblocking = ScopeGuard([&] { MUST(m_socket->set_blocking(false)); });
        MUST(m_socket->write_until_depleted(packet.bytes()));
    }

    JsonObject request(StringView to, StringView type)
    {
        JsonObject message;
        message.set("to"sv, to);
        message.set("type"sv, type);
        return request(move(message), to);
    }

    JsonObject request(JsonObject message)
    {
        auto to = message.get_string("to"sv).value_or("root"_string);
        return request(move(message), to);
    }

private:
    ProtocolClient(Core::EventLoop& loop, NonnullOwnPtr<Core::TCPSocket> socket)
        : m_loop(loop)
        , m_socket(move(socket))
    {
    }

    static bool is_event_packet(JsonObject const& packet)
    {
        auto type = packet.get_string("type"sv);
        if (!type.has_value())
            return false;
        return *type == "frameUpdate"sv
            || *type == "resources-available-array"sv
            || *type == "resources-updated-array"sv
            || *type == "newMutations"sv
            || *type == "tabListChanged"sv;
    }

    JsonObject request(JsonObject message, StringView response_actor)
    {
        send(move(message));
        while (true) {
            auto response = read_message_from_socket();
            if (response.get_string("from"sv).value_or({}) == response_actor && !is_event_packet(response))
                return response;
            m_pending_messages.append(move(response));
        }
    }

    JsonObject read_message_from_socket()
    {
        Optional<JsonObject> message;
        spin_until(m_loop, [&] {
            pump(m_loop);
            if (!MUST(m_socket->can_read_without_blocking()))
                return false;
            message = read_message_now();
            return true;
        });
        return message.release_value();
    }

    JsonObject read_message_now()
    {
        MUST(m_socket->set_blocking(true));
        auto restore_nonblocking = ScopeGuard([&] { MUST(m_socket->set_blocking(false)); });

        ByteBuffer length_buffer;
        while (true) {
            auto byte = MUST(m_socket->read_value<u8>());
            if (byte == ':')
                break;
            length_buffer.append(byte);
        }

        auto length = StringView { length_buffer }.to_number<size_t>();
        VERIFY(length.has_value());

        ByteBuffer message_buffer;
        message_buffer.resize(*length);
        MUST(m_socket->read_until_filled(message_buffer));

        auto value = JsonValue::from_string(message_buffer);
        VERIFY(!value.is_error());
        VERIFY(value.value().is_object());
        return value.release_value().as_object();
    }

    Core::EventLoop& m_loop;
    NonnullOwnPtr<Core::TCPSocket> m_socket;
    Vector<JsonObject> m_pending_messages;
};

struct TestSession {
    Core::EventLoop loop;
    TestDevToolsDelegate delegate;
    OwnPtr<DevTools::DevToolsServer> server;
    OwnPtr<ProtocolClient> client;
};

static NonnullOwnPtr<TestSession> create_session()
{
    auto session = make<TestSession>();
    session->server = MUST(DevTools::DevToolsServer::create(session->delegate, 0));
    session->client = ProtocolClient::connect(session->loop, *session->server);
    return session;
}

static String actor_from(JsonObject const& object, StringView key)
{
    return object.get_string(key).release_value();
}

static JsonObject get_tab(ProtocolClient& client)
{
    auto tabs = client.request("root"sv, "listTabs"sv).get_array("tabs"sv).release_value();
    VERIFY(tabs.size() == 1u);
    return tabs.at(0).as_object();
}

static JsonObject get_frame_target(ProtocolClient& client, StringView tab_actor)
{
    auto watcher_actor = actor_from(client.request(tab_actor, "getWatcher"sv), "actor"sv);

    JsonObject request;
    request.set("to"sv, watcher_actor);
    request.set("type"sv, "watchTargets"sv);
    request.set("targetType"sv, "frame"sv);

    auto response = client.request(move(request));
    VERIFY(response.get_string("type"sv).value() == "target-available-form"sv);
    return response.get_object("target"sv).release_value();
}

static JsonObject get_walker(ProtocolClient& client, StringView inspector_actor)
{
    return client.request(inspector_actor, "getWalker"sv).get_object("walker"sv).release_value();
}

static String query_selector(ProtocolClient& client, StringView walker_actor, StringView root_node, StringView selector)
{
    JsonObject request;
    request.set("to"sv, walker_actor);
    request.set("type"sv, "querySelector"sv);
    request.set("node"sv, root_node);
    request.set("selector"sv, selector);
    return client.request(move(request)).get_object("node"sv)->get_string("actor"sv).release_value();
}

static JsonObject read_resource(ProtocolClient& client, StringView resource_type, StringView packet_type = "resources-available-array"sv)
{
    while (true) {
        auto message = client.read_message();
        if (message.get_string("type"sv).value_or({}) != packet_type)
            continue;

        auto array = message.get_array("array"sv);
        if (!array.has_value())
            continue;

        for (auto const& entry : array->values()) {
            if (!entry.is_array() || entry.as_array().size() < 2)
                continue;
            if (!entry.as_array().at(0).is_string() || entry.as_array().at(0).as_string() != resource_type)
                continue;
            auto const& resources = entry.as_array().at(1).as_array();
            VERIFY(!resources.is_empty());
            return resources.at(0).as_object();
        }
    }
}

TEST_CASE(root_actor_and_connection_errors)
{
    auto session = create_session();
    auto& client = *session->client;

    auto greeting = client.read_message();
    EXPECT_EQ(greeting.get_string("applicationType"sv).value(), "browser"sv);
    auto traits = greeting.get_object("traits"sv).release_value();
    EXPECT(traits.get_bool("highlightable"sv).value());
    EXPECT(traits.get_bool("customHighlighters"sv).value());
    EXPECT(traits.get_bool("networkMonitor"sv).value());

    EXPECT_EQ(client.request("root"sv, "connect"sv).get_string("from"sv).value(), "root"sv);
    auto root = client.request("root"sv, "getRoot"sv);
    EXPECT(root.has_string("deviceActor"sv));
    EXPECT(root.has_string("parentAccessibilityActor"sv));
    EXPECT(root.has_string("preferenceActor"sv));

    EXPECT_EQ(client.request("root"sv, "listProcesses"sv).get_array("processes"sv)->size(), 1u);
    EXPECT(client.request("root"sv, "listAddons"sv).get_array("addons"sv)->is_empty());
    EXPECT(client.request("root"sv, "listWorkers"sv).get_array("workers"sv)->is_empty());
    EXPECT(client.request("root"sv, "listServiceWorkerRegistrations"sv).get_array("registrations"sv)->is_empty());

    auto tab = get_tab(client);
    auto tab_actor = actor_from(tab, "actor"sv);
    EXPECT_EQ(tab.get_string("title"sv).value(), "Fixture page"sv);
    EXPECT_EQ(tab.get_integer<u64>("browserId"sv).value(), 1u);

    JsonObject get_tab_request;
    get_tab_request.set("to"sv, "root"sv);
    get_tab_request.set("type"sv, "getTab"sv);
    get_tab_request.set("browserId"sv, 1);
    EXPECT_EQ(client.request(move(get_tab_request)).get_object("tab"sv)->get_string("actor"sv).value(), tab_actor);

    session->server->refresh_tab_list();
    EXPECT_EQ(client.read_message().get_string("type"sv).value(), "tabListChanged"sv);
    session->server->refresh_tab_list();
    pump(session->loop);

    JsonObject missing_to;
    missing_to.set("type"sv, "connect"sv);
    client.send(move(missing_to));
    EXPECT_EQ(client.read_message().get_string("error"sv).value(), "missingParameter"sv);

    JsonObject missing_type;
    missing_type.set("to"sv, "root"sv);
    client.send(move(missing_type));
    EXPECT_EQ(client.read_message().get_string("error"sv).value(), "missingParameter"sv);

    JsonObject unknown_actor;
    unknown_actor.set("to"sv, "missing"sv);
    unknown_actor.set("type"sv, "connect"sv);
    client.send(move(unknown_actor));
    EXPECT_EQ(client.read_message().get_string("error"sv).value(), "unknownActor"sv);
    EXPECT_EQ(client.request("root"sv, "notARealPacket"sv).get_string("error"sv).value(), "unrecognizedPacketType"sv);

    JsonObject first;
    first.set("to"sv, "root"sv);
    first.set("type"sv, "listWorkers"sv);
    JsonObject second;
    second.set("to"sv, "root"sv);
    second.set("type"sv, "listAddons"sv);
    client.send(move(first));
    client.send(move(second));
    EXPECT(client.read_message().has_array("workers"sv));
    EXPECT(client.read_message().has_array("addons"sv));
}

TEST_CASE(target_bootstrap_and_lifetime)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto tab_actor = actor_from(get_tab(client), "actor"sv);
    EXPECT(client.request(tab_actor, "getFavicon"sv).get("favicon"sv).value().is_null());

    auto watcher_actor = actor_from(client.request(tab_actor, "getWatcher"sv), "actor"sv);
    EXPECT_EQ(actor_from(client.request(tab_actor, "getWatcher"sv), "actor"sv), watcher_actor);
    EXPECT(client.request(watcher_actor, "getTargetConfigurationActor"sv).has_object("configuration"sv));
    EXPECT(client.request(watcher_actor, "getThreadConfigurationActor"sv).has_object("configuration"sv));
    EXPECT(client.request(watcher_actor, "getNetworkParentActor"sv).has_string("network"sv));

    JsonObject watch_resources;
    watch_resources.set("to"sv, watcher_actor);
    watch_resources.set("type"sv, "watchResources"sv);
    JsonArray resource_types;
    resource_types.must_append("console-message"sv);
    watch_resources.set("resourceTypes"sv, move(resource_types));
    EXPECT_EQ(client.request(move(watch_resources)).get_string("from"sv).value(), watcher_actor);

    auto target = get_frame_target(client, tab_actor);
    EXPECT_EQ(target.get_string("targetType"sv).value(), "frame"sv);
    EXPECT(target.has_string("consoleActor"sv));
    EXPECT(target.has_string("inspectorActor"sv));
    EXPECT(target.has_string("styleSheetsActor"sv));
    EXPECT(target.has_string("threadActor"sv));
    EXPECT(target.has_string("accessibilityActor"sv));
    EXPECT_EQ(session->delegate.did_connect_devtools_client_call_count, 1u);
    EXPECT_EQ(session->delegate.listen_for_console_messages_call_count, 1u);
    EXPECT_EQ(session->delegate.listen_for_network_events_call_count, 1u);
    EXPECT_EQ(session->delegate.retrieve_style_sheets_call_count, 1u);

    EXPECT_EQ(client.request(actor_from(target, "actor"sv), "detach"sv).get_string("from"sv).value(), actor_from(target, "actor"sv));
    EXPECT_EQ(session->delegate.stop_listening_for_console_messages_call_count, 1u);
    EXPECT_EQ(session->delegate.stop_listening_for_dom_mutations_call_count, 1u);
    EXPECT_EQ(session->delegate.clear_highlighted_dom_node_call_count, 1u);
    EXPECT_EQ(session->delegate.clear_inspected_dom_node_call_count, 1u);
}

TEST_CASE(inspector_walker_highlighter_layout_and_editing)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto inspector_actor = actor_from(target, "inspectorActor"sv);
    auto walker = get_walker(client, inspector_actor);
    auto walker_actor = actor_from(walker, "actor"sv);
    auto root_node = walker.get_object("root"sv).release_value();
    auto root_node_actor = actor_from(root_node, "actor"sv);

    EXPECT_EQ(session->delegate.inspect_tab_call_count, 1u);
    EXPECT(root_node.get_bool("isTopLevelDocument"sv).value());
    EXPECT_EQ(root_node.get_string("displayType"sv).value(), "block"sv);

    JsonObject children;
    children.set("to"sv, walker_actor);
    children.set("type"sv, "children"sv);
    children.set("node"sv, root_node_actor);
    auto children_response = client.request(move(children));
    EXPECT(children_response.get_bool("hasFirst"sv).value());
    EXPECT_EQ(children_response.get_array("nodes"sv)->size(), 1u);

    auto div_actor = query_selector(client, walker_actor, root_node_actor, "div"sv);
    JsonObject previous_sibling;
    previous_sibling.set("to"sv, walker_actor);
    previous_sibling.set("type"sv, "previousSibling"sv);
    previous_sibling.set("node"sv, div_actor);
    EXPECT_EQ(client.request(move(previous_sibling)).get_object("node"sv)->get_string("displayName"sv).value(), "span"sv);

    JsonObject is_in_dom_tree;
    is_in_dom_tree.set("to"sv, walker_actor);
    is_in_dom_tree.set("type"sv, "isInDOMTree"sv);
    is_in_dom_tree.set("node"sv, div_actor);
    EXPECT(client.request(move(is_in_dom_tree)).get_bool("attached"sv).value());

    JsonObject watch_root;
    watch_root.set("to"sv, walker_actor);
    watch_root.set("type"sv, "watchRootNode"sv);
    EXPECT_EQ(client.request(move(watch_root)).get_string("type"sv).value(), "root-available"sv);
    while (true) {
        auto message = client.read_message();
        if (message.get_string("from"sv).value_or({}) == walker_actor && !message.has_string("type"sv))
            break;
    }

    EXPECT(client.request(walker_actor, "retainNode"sv).has_string("from"sv));
    EXPECT(client.request(walker_actor, "getOffsetParent"sv).get("node"sv).value().is_null());

    JsonObject highlighter_request;
    highlighter_request.set("to"sv, inspector_actor);
    highlighter_request.set("type"sv, "getHighlighterByType"sv);
    highlighter_request.set("typeName"sv, "BoxModelHighlighter"sv);
    auto highlighter_actor = client.request(move(highlighter_request)).get_object("highlighter"sv)->get_string("actor"sv).release_value();

    JsonObject second_highlighter_request;
    second_highlighter_request.set("to"sv, inspector_actor);
    second_highlighter_request.set("type"sv, "getHighlighterByType"sv);
    second_highlighter_request.set("typeName"sv, "BoxModelHighlighter"sv);
    auto second_highlighter_response = client.request(move(second_highlighter_request));
    VERIFY(second_highlighter_response.has_object("highlighter"sv));
    EXPECT_EQ(second_highlighter_response.get_object("highlighter"sv)->get_string("actor"sv).value(), highlighter_actor);

    JsonObject show_highlighter;
    show_highlighter.set("to"sv, highlighter_actor);
    show_highlighter.set("type"sv, "show"sv);
    show_highlighter.set("node"sv, div_actor);
    EXPECT(client.request(move(show_highlighter)).get_bool("value"sv).value());
    EXPECT_EQ(session->delegate.highlight_dom_node_call_count, 1u);
    EXPECT_EQ(session->delegate.last_highlighted_dom_node.value(), 4u);

    JsonObject show_unknown;
    show_unknown.set("to"sv, highlighter_actor);
    show_unknown.set("type"sv, "show"sv);
    show_unknown.set("node"sv, "missing-actor"sv);
    EXPECT(!client.request(move(show_unknown)).get_bool("value"sv).value());
    EXPECT_EQ(client.request(highlighter_actor, "hide"sv).get_string("from"sv).value(), highlighter_actor);

    auto layout_actor = client.request(walker_actor, "getLayoutInspector"sv).get_object("actor"sv)->get_string("actor"sv).release_value();
    EXPECT(client.request(layout_actor, "getGrids"sv).get_array("grids"sv)->is_empty());
    EXPECT(client.request(layout_actor, "getCurrentFlexbox"sv).get("flexbox"sv).value().is_null());

    JsonObject set_outer_html;
    set_outer_html.set("to"sv, walker_actor);
    set_outer_html.set("type"sv, "setOuterHTML"sv);
    set_outer_html.set("node"sv, div_actor);
    set_outer_html.set("value"sv, "<section></section>"sv);
    EXPECT(client.request(move(set_outer_html)).has_string("from"sv));
    EXPECT_EQ(session->delegate.set_dom_node_outer_html_call_count, 1u);
    EXPECT_EQ(session->delegate.last_html.value(), "<section></section>"sv);

    JsonObject edit_tag;
    edit_tag.set("to"sv, walker_actor);
    edit_tag.set("type"sv, "editTagName"sv);
    edit_tag.set("node"sv, div_actor);
    edit_tag.set("tagName"sv, "section"sv);
    EXPECT(client.request(move(edit_tag)).has_string("from"sv));
    EXPECT_EQ(session->delegate.set_dom_node_tag_call_count, 1u);

    JsonObject duplicate;
    duplicate.set("to"sv, walker_actor);
    duplicate.set("type"sv, "duplicateNode"sv);
    duplicate.set("node"sv, div_actor);
    EXPECT(client.request(move(duplicate)).has_string("from"sv));
    EXPECT_EQ(session->delegate.clone_dom_node_call_count, 1u);

    JsonObject mutation_target = make_node(4, "element"sv, "DIV"sv);
    JsonObject attributes;
    attributes.set("id"sv, "target"sv);
    attributes.set("class"sv, "updated"sv);
    mutation_target.set("attributes"sv, move(attributes));

    WebView::Mutation mutation { "attributes"_string, 4, mutation_target.serialized(), WebView::AttributeMutation { "class"_string, "updated"_string } };
    session->delegate.emit_mutation(move(mutation));
    EXPECT_EQ(client.read_message().get_string("type"sv).value(), "newMutations"sv);

    auto mutations = client.request(walker_actor, "getMutations"sv).get_array("mutations"sv).release_value();
    VERIFY(mutations.size() == 1u);
    EXPECT_EQ(mutations.at(0).as_object().get_string("type"sv).value(), "attributes"sv);
    EXPECT_EQ(mutations.at(0).as_object().get_string("newValue"sv).value(), "updated"sv);
}

TEST_CASE(styles_and_stylesheets)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto inspector_actor = actor_from(target, "inspectorActor"sv);
    auto style_sheets_actor = actor_from(target, "styleSheetsActor"sv);

    auto page_style = client.request(inspector_actor, "getPageStyle"sv).get_object("pageStyle"sv).release_value();
    auto page_style_actor = actor_from(page_style, "actor"sv);
    EXPECT(page_style.get_object("traits"sv)->get_bool("fontVariations"sv).value());

    auto walker = get_walker(client, inspector_actor);
    auto walker_actor = actor_from(walker, "actor"sv);
    auto root_actor = walker.get_object("root"sv)->get_string("actor"sv).release_value();
    auto div_actor = query_selector(client, walker_actor, root_actor, "div"sv);

    JsonObject applied;
    applied.set("to"sv, page_style_actor);
    applied.set("type"sv, "getApplied"sv);
    EXPECT(client.request(move(applied)).get_array("entries"sv)->is_empty());
    EXPECT(!client.request(page_style_actor, "isPositionEditable"sv).get_bool("value"sv).value());

    JsonObject computed_request;
    computed_request.set("to"sv, page_style_actor);
    computed_request.set("type"sv, "getComputed"sv);
    computed_request.set("node"sv, div_actor);
    auto computed = client.request(move(computed_request)).get_object("computed"sv).release_value();
    EXPECT_EQ(computed.get_object("display"sv)->get_string("value"sv).value(), "block"sv);

    JsonObject layout_request;
    layout_request.set("to"sv, page_style_actor);
    layout_request.set("type"sv, "getLayout"sv);
    layout_request.set("node"sv, div_actor);
    auto layout = client.request(move(layout_request));
    EXPECT_EQ(layout.get_string("width"sv).value(), "100px"sv);
    EXPECT_EQ(layout.get_string("margin-top"sv).value(), "5px"sv);

    JsonObject font_request;
    font_request.set("to"sv, page_style_actor);
    font_request.set("type"sv, "getUsedFontFaces"sv);
    font_request.set("node"sv, div_actor);
    auto fonts = client.request(move(font_request)).get_array("fontFaces"sv).release_value();
    VERIFY(fonts.size() == 1u);
    EXPECT_EQ(fonts.at(0).as_object().get_string("name"sv).value(), "Test Sans"sv);

    auto style_resources = client.read_message();
    while (style_resources.get_string("type"sv).value_or({}) != "resources-available-array"sv)
        style_resources = client.read_message();
    auto sheets = style_resources.get_array("array"sv)->at(0).as_array().at(1).as_array();
    VERIFY(sheets.size() == 1u);
    auto resource_id = sheets.at(0).as_object().get_string("resourceId"sv).release_value();

    JsonObject get_text;
    get_text.set("to"sv, style_sheets_actor);
    get_text.set("type"sv, "getText"sv);
    get_text.set("resourceId"sv, resource_id);
    EXPECT_EQ(client.request(move(get_text)).get_string("text"sv).value(), "body { color: red; }"sv);

    JsonObject bad_get_text;
    bad_get_text.set("to"sv, style_sheets_actor);
    bad_get_text.set("type"sv, "getText"sv);
    bad_get_text.set("resourceId"sv, "missing:99"sv);
    EXPECT_EQ(client.request(move(bad_get_text)).get_string("error"sv).value(), "unknownActor"sv);
}

TEST_CASE(console_network_navigation_and_accessibility)
{
    auto session = create_session();
    auto& client = *session->client;
    (void)client.read_message();

    auto target = get_frame_target(client, actor_from(get_tab(client), "actor"sv));
    auto accessibility_actor = actor_from(target, "accessibilityActor"sv);

    session->delegate.emit_console_log(JS::Console::LogLevel::Warn, { "careful"_string });
    auto warning = read_resource(client, "console-message"sv);
    EXPECT_EQ(warning.get_string("level"sv).value(), "warn"sv);

    session->delegate.emit_console_trace();
    auto trace = read_resource(client, "console-message"sv);
    EXPECT_EQ(trace.get_string("level"sv).value(), "trace"sv);
    EXPECT_EQ(trace.get_array("stacktrace"sv)->at(0).as_object().get_string("functionName"sv).value(), "traceFunction"sv);

    session->delegate.emit_console_error();
    auto page_error = read_resource(client, "error-message"sv).get_object("pageError"sv).release_value();
    EXPECT(page_error.get_bool("isPromiseRejection"sv).value());

    session->delegate.emit_network_lifecycle();
    auto network_event = read_resource(client, "network-event"sv);
    EXPECT_EQ(network_event.get_string("method"sv).value(), "POST"sv);
    EXPECT(network_event.get_bool("isXHR"sv).value());
    auto network_actor = network_event.get_string("actor"sv).release_value();
    (void)read_resource(client, "network-event"sv, "resources-updated-array"sv);
    (void)read_resource(client, "network-event"sv, "resources-updated-array"sv);

    JsonObject content_request;
    content_request.set("to"sv, network_actor);
    content_request.set("type"sv, "getResponseContent"sv);
    auto content = client.request(move(content_request)).get_object("content"sv).release_value();
    EXPECT_EQ(content.get_string("text"sv).value(), "{\"ok\":true}"sv);
    EXPECT_EQ(content.get_string("mimeType"sv).value(), "application/json"sv);

    session->delegate.emit_navigation();
    EXPECT_EQ(read_resource(client, "document-event"sv).get_string("name"sv).value(), "will-navigate"sv);
    while (true) {
        auto packet = client.read_message();
        if (packet.get_string("type"sv).value_or({}) == "tabNavigated"sv && packet.get_string("state"sv).value_or({}) == "stop"sv)
            break;
    }

    EXPECT(client.request(accessibility_actor, "bootstrap"sv).has_object("state"sv));
    EXPECT(client.request(accessibility_actor, "getTraits"sv).get_object("traits"sv)->get_bool("tabbingOrder"sv).value());
    EXPECT(client.request(accessibility_actor, "getSimulator"sv).get("simulator"sv).value().is_null());

    auto accessibility_walker = client.request(accessibility_actor, "getWalker"sv).get_object("walker"sv)->get_string("actor"sv).release_value();
    auto accessibility_root = client.request(accessibility_walker, "children"sv).get_array("children"sv)->at(0).as_object().get_string("actor"sv).release_value();
    auto accessibility_button = client.request(accessibility_root, "children"sv).get_array("children"sv)->at(0).as_object().get_string("actor"sv).release_value();

    auto walker = get_walker(client, actor_from(target, "inspectorActor"sv));
    JsonObject node_from_accessibility;
    node_from_accessibility.set("to"sv, actor_from(walker, "actor"sv));
    node_from_accessibility.set("type"sv, "getNodeFromActor"sv);
    node_from_accessibility.set("actorID"sv, accessibility_button);
    JsonArray path;
    path.must_append("rawAccessible"sv);
    path.must_append("DOMNode"sv);
    node_from_accessibility.set("path"sv, move(path));
    auto node_response = client.request(move(node_from_accessibility));
    EXPECT_EQ(node_response.get_object("node"sv)->get_object("node"sv)->get_string("displayName"sv).value(), "div"sv);
}
