/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Promise.h>
#include <LibWebCommon/WebDriver/Error.h>
#include <WebDriver/WebSocket.h>

namespace WebDriver {

class Session;

// https://w3c.github.io/webdriver-bidi/#bidi-session
// NB: The state a session has once its BiDi flag is set.
class BiDiSession {
    AK_MAKE_NONCOPYABLE(BiDiSession);
    AK_MAKE_NONMOVABLE(BiDiSession);
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit BiDiSession(Session&);
    ~BiDiSession();

    // https://w3c.github.io/webdriver-bidi/#start-listening-for-a-websocket-connection
    static ErrorOr<String> start_listening_for_a_websocket_connection(Session const&);

    void did_receive_event(String const& name, JsonValue params, Vector<String> const& top_level_traversable_ids, Vector<String> const& user_context_ids);
    void did_close_top_level_traversable(String const& id);

    // https://w3c.github.io/webdriver-bidi/#close-the-websocket-connections
    void close_the_websocket_connections();

private:
    using WebDriverPromise = Core::Promise<JsonValue, Web::WebDriver::Error>;

    // https://w3c.github.io/webdriver-bidi/#subscription
    struct Subscription {
        String subscription_id;
        HashTable<String> event_names;
        HashTable<String> top_level_traversable_ids;
        HashTable<String> user_context_ids;

        // https://w3c.github.io/webdriver-bidi/#subscription-global
        bool is_global() const { return top_level_traversable_ids.is_empty() && user_context_ids.is_empty(); }
    };

    static bool accept_connection(WebSocketConnection&, StringView resource_name);
    void add_connection(NonnullRefPtr<WebSocketConnection>);
    void handle_a_connection_closing(WebSocketConnection&);

    void handle_an_incoming_message(WebSocketConnection&, WebSocketConnection::MessageType, ByteBuffer data);
    NonnullRefPtr<WebDriverPromise> run_command(StringView method, JsonValue params);

    void send_an_error_response(WebSocketConnection&, JsonValue command_id, Web::WebDriver::Error const&);
    void send_a_success_response(WebSocketConnection&, JsonValue command_id, JsonValue result);
    void emit_an_event(StringView name, JsonValue params);

    bool event_is_enabled(StringView event_name, Vector<String> const& top_level_traversable_ids, Vector<String> const& user_context_ids) const;
    HashTable<String> set_of_top_level_traversables_for_which_an_event_is_enabled(StringView event_name) const;
    void update_enabled_events();
    void remove_destroyed_navigable_from_subscriptions(String const& id);
    void buffer_a_log_event(Vector<String> const& top_level_traversable_ids, JsonValue event);
    void run_log_entry_added_subscribe_steps(HashTable<String> const& navigables, bool include_global);

    NonnullRefPtr<WebDriverPromise> session_status();
    NonnullRefPtr<WebDriverPromise> session_end();
    NonnullRefPtr<WebDriverPromise> session_subscribe(JsonValue params);
    NonnullRefPtr<WebDriverPromise> session_unsubscribe(JsonValue params);

    NonnullRefPtr<WebDriverPromise> complete_subscribe(HashTable<String> event_names, HashTable<String> subscription_navigables, HashTable<String> top_level_traversable_context_ids, HashTable<String> input_user_context_ids);
    void run_subscribe_steps(Vector<String> event_names, HashMap<String, HashTable<String>> subscribe_step_events, bool include_global, NonnullRefPtr<WebDriverPromise>, JsonValue body);

    Session& m_session;
    bool m_has_ended { false };

    // https://w3c.github.io/webdriver-bidi/#session-websocket-connections
    Vector<NonnullRefPtr<WebSocketConnection>> m_session_websocket_connections;

    // https://w3c.github.io/webdriver-bidi/#subscriptions
    Vector<Subscription> m_subscriptions;

    // https://w3c.github.io/webdriver-bidi/#known-subscription-ids
    HashTable<String> m_known_subscription_ids;

    HashTable<String> m_enabled_events;

    // https://w3c.github.io/webdriver-bidi/#log-event-buffer
    struct BufferedLogEvent {
        JsonValue event;
        size_t size { 0 };
    };
    struct BufferedLogEvents {
        Vector<BufferedLogEvent> events;
        size_t size { 0 };
    };
    HashMap<String, BufferedLogEvents> m_log_event_buffer;
};

}
