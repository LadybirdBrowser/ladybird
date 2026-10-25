/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibWebCommon/WebDriver/BiDi.h>
#include <WebDriver/BiDiSession.h>
#include <WebDriver/Session.h>

// Failures become rejected promises rather than propagated ErrorOr values, so this macro may only be used in functions
// returning a WebDriverPromise.
#define TRY_OR_REJECT(expression)                                                                    \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (expression));                                                \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        if (_temporary_result.is_error()) [[unlikely]] {                                             \
            return WebDriverPromise::rejected(_temporary_result.release_error());                    \
        }                                                                                            \
        _temporary_result.release_value();                                                           \
    })

namespace WebDriver {

// https://w3c.github.io/webdriver-bidi/#set-of-all-command-names
static constexpr auto s_command_names = to_array<StringView>({
    "browser.close"sv,
    "browser.createUserContext"sv,
    "browser.getClientWindows"sv,
    "browser.getUserContexts"sv,
    "browser.removeUserContext"sv,
    "browser.setClientWindowState"sv,
    "browser.setDownloadBehavior"sv,
    "browsingContext.activate"sv,
    "browsingContext.captureScreenshot"sv,
    "browsingContext.close"sv,
    "browsingContext.create"sv,
    "browsingContext.getTree"sv,
    "browsingContext.handleUserPrompt"sv,
    "browsingContext.locateNodes"sv,
    "browsingContext.navigate"sv,
    "browsingContext.print"sv,
    "browsingContext.reload"sv,
    "browsingContext.setBypassCSP"sv,
    "browsingContext.setViewport"sv,
    "browsingContext.startScreencast"sv,
    "browsingContext.stopScreencast"sv,
    "browsingContext.traverseHistory"sv,
    "emulation.setForcedColorsModeThemeOverride"sv,
    "emulation.setGeolocationOverride"sv,
    "emulation.setLocaleOverride"sv,
    "emulation.setMediaFeaturesOverride"sv,
    "emulation.setNetworkConditions"sv,
    "emulation.setScreenSettingsOverride"sv,
    "emulation.setScreenOrientationOverride"sv,
    "emulation.setUserAgentOverride"sv,
    "emulation.setViewportMetaOverride"sv,
    "emulation.setScriptingEnabled"sv,
    "emulation.setScrollbarTypeOverride"sv,
    "emulation.setTextLayoutModeOverride"sv,
    "emulation.setTimezoneOverride"sv,
    "emulation.setTouchOverride"sv,
    "input.performActions"sv,
    "input.releaseActions"sv,
    "input.setFiles"sv,
    "network.addDataCollector"sv,
    "network.addIntercept"sv,
    "network.continueRequest"sv,
    "network.continueResponse"sv,
    "network.continueWithAuth"sv,
    "network.disownData"sv,
    "network.failRequest"sv,
    "network.getData"sv,
    "network.provideResponse"sv,
    "network.removeDataCollector"sv,
    "network.removeIntercept"sv,
    "network.setCacheBehavior"sv,
    "network.setExtraHeaders"sv,
    "script.addPreloadScript"sv,
    "script.callFunction"sv,
    "script.disown"sv,
    "script.evaluate"sv,
    "script.getRealms"sv,
    "script.removePreloadScript"sv,
    "session.end"sv,
    "session.new"sv,
    "session.status"sv,
    "session.subscribe"sv,
    "session.unsubscribe"sv,
    "storage.deleteCookies"sv,
    "storage.getCookies"sv,
    "storage.setCookie"sv,
    "webExtension.install"sv,
    "webExtension.uninstall"sv,
});

// https://w3c.github.io/webdriver-bidi/#module-name
static constexpr auto s_module_names = to_array<StringView>({
    "browser"sv,
    "browsingContext"sv,
    "emulation"sv,
    "input"sv,
    "log"sv,
    "network"sv,
    "script"sv,
    "session"sv,
    "storage"sv,
    "webExtension"sv,
});

// https://w3c.github.io/webdriver-bidi/#event-name
static constexpr auto s_event_names = to_array<StringView>({
    "browsingContext.contextCreated"sv,
    "browsingContext.contextDestroyed"sv,
    "browsingContext.domContentLoaded"sv,
    "browsingContext.downloadEnd"sv,
    "browsingContext.downloadWillBegin"sv,
    "browsingContext.fragmentNavigated"sv,
    "browsingContext.historyUpdated"sv,
    "browsingContext.load"sv,
    "browsingContext.navigationAborted"sv,
    "browsingContext.navigationCommitted"sv,
    "browsingContext.navigationFailed"sv,
    "browsingContext.navigationStarted"sv,
    "browsingContext.userPromptClosed"sv,
    "browsingContext.userPromptOpened"sv,
    "input.fileDialogOpened"sv,
    "log.entryAdded"sv,
    "network.authRequired"sv,
    "network.beforeRequestSent"sv,
    "network.fetchError"sv,
    "network.responseCompleted"sv,
    "network.responseStarted"sv,
    "script.message"sv,
    "script.realmCreated"sv,
    "script.realmDestroyed"sv,
});

// https://w3c.github.io/webdriver-bidi/#remote-end-subscribe-steps
// NB: The events that define remote end subscribe steps, with their subscribe priority.
struct EventWithSubscribeSteps {
    StringView name;
    int subscribe_priority;
};
static constexpr auto s_events_with_subscribe_steps = to_array<EventWithSubscribeSteps>({
    { "browsingContext.contextCreated"sv, 1 },
    { "script.realmCreated"sv, 2 },
    { "log.entryAdded"sv, 10 },
});

// https://w3c.github.io/webdriver-bidi/#log-event-buffer
// User agents may impose a maximum size on this buffer, subject to the condition that if events A and B happen in the
// same context with A occurring before B, and both are added to the buffer, the entry for B must not be removed before
// the entry for A.
static constexpr size_t MAXIMUM_LOG_EVENT_BUFFER_SIZE = 1000;

// NB: Web content decides how large its log events are, so the events buffered for a navigable are bounded in bytes
//     too.
static constexpr size_t MAXIMUM_LOG_EVENT_BUFFER_BYTES = 16 * MiB;

static Optional<int> subscribe_priority(StringView event_name)
{
    for (auto const& event : s_events_with_subscribe_steps) {
        if (event.name == event_name)
            return event.subscribe_priority;
    }
    return {};
}

// https://w3c.github.io/webdriver-bidi/#default-user-context
// FIXME: Support user contexts other than the default user context.
static constexpr auto DEFAULT_USER_CONTEXT_ID = "default"sv;

// NB: A response of the browser process that does not have the form of the result of its command.
static Web::WebDriver::Error invalid_browser_response()
{
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Invalid response from the browser"sv);
}

static Web::WebDriver::Error invalid_argument(StringView message)
{
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, message);
}

// https://w3c.github.io/webdriver-bidi/#obtain-a-set-of-event-names
static ErrorOr<HashTable<String>, Web::WebDriver::Error> obtain_a_set_of_event_names(String const& name)
{
    // 1. Let events be an empty set.
    HashTable<String> events;

    // 2. If name contains a U+002E (period):
    if (name.contains('.')) {
        // 1. If name is the event name for an event, append name to events and return success with data events.
        if (any_of(s_event_names, [&](auto event_name) { return event_name == name; })) {
            events.set(name);
            return events;
        }

        // 2. Return an error with error code invalid argument
        return invalid_argument("Unknown event name"sv);
    }

    // 3. Otherwise name is interpreted as representing all the events in a module. If name is not a module name return
    //    an error with error code invalid argument.
    if (!any_of(s_module_names, [&](auto module_name) { return module_name == name; }))
        return invalid_argument("Unknown module name"sv);

    // 4. Append the event name for each event in the module with name name to events.
    for (auto event_name : s_event_names) {
        if (event_name.starts_with(name) && event_name.length() > name.bytes_as_string_view().length() && event_name[name.bytes_as_string_view().length()] == '.')
            events.set(MUST(String::from_utf8(event_name)));
    }

    // 5. Return success with data events.
    return events;
}

static bool sets_are_equal(HashTable<String> const& a, HashTable<String> const& b)
{
    return a.size() == b.size() && all_of(a, [&](auto const& item) { return b.contains(item); });
}

// https://w3c.github.io/webdriver-bidi/#get-a-session-id-for-a-websocket-resource
static Optional<StringView> get_a_session_id_for_a_websocket_resource(StringView resource_name)
{
    // 1. If resource name doesn't begin with the byte string "/session/", return null.
    if (!resource_name.starts_with("/session/"sv))
        return {};

    // 2. Let session id be the bytes in resource name following the "/session/" prefix.
    auto session_id = resource_name.substring_view("/session/"sv.length());

    // 3. If session id is not the string representation of a UUID, return null.
    if (session_id.length() != 36)
        return {};
    for (size_t i = 0; i < session_id.length(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (session_id[i] != '-')
                return {};
        } else if (!is_ascii_hex_digit(session_id[i])) {
            return {};
        }
    }

    // 4. Return session id.
    return session_id;
}

// https://w3c.github.io/webdriver-bidi/#active-listeners
// NB: An endpoint node supports one session at a time, so a single listener serves every session.
static OwnPtr<WebSocketListener>& websocket_listener()
{
    static NeverDestroyed<OwnPtr<WebSocketListener>> listener;
    return *listener;
}

// https://w3c.github.io/webdriver-bidi/#websocket-connections-not-associated-with-a-session
// NB: We do not support BiDi-only sessions, so this only holds connections whose opening handshake is not complete.
static Vector<NonnullRefPtr<WebSocketConnection>>& connections_awaiting_opening_handshake()
{
    static NeverDestroyed<Vector<NonnullRefPtr<WebSocketConnection>>> connections;
    return *connections;
}

// NB: The connections whose closing handshake we started, until it completes.
static Vector<NonnullRefPtr<WebSocketConnection>>& closing_connections()
{
    static NeverDestroyed<Vector<NonnullRefPtr<WebSocketConnection>>> connections;
    return *connections;
}

// NB: A WebDriver client opens one connection at a time. This bounds the connections that other local processes can
//     keep open without completing an opening handshake.
static constexpr size_t MAXIMUM_CONNECTIONS_AWAITING_OPENING_HANDSHAKE = 16;

BiDiSession::BiDiSession(Session& session)
    : m_session(session)
{
}

BiDiSession::~BiDiSession()
{
    close_the_websocket_connections();
}

// https://w3c.github.io/webdriver-bidi/#start-listening-for-a-websocket-connection
ErrorOr<String> BiDiSession::start_listening_for_a_websocket_connection(Session const& session)
{
    // 1. If there is an existing WebSocket listener in active listeners which the remote end would like to reuse, let
    //    listener be that listener. Otherwise let listener be a new WebSocket listener with implementation-defined host,
    //    port, secure flag, and an empty list of WebSocket resources.
    if (!websocket_listener()) {
        websocket_listener() = TRY(WebSocketListener::create());
        websocket_listener()->on_connection = [](NonnullRefPtr<WebSocketConnection> connection) {
            if (connections_awaiting_opening_handshake().size() >= MAXIMUM_CONNECTIONS_AWAITING_OPENING_HANDSHAKE)
                return;
            connection->on_opening_handshake = [connection = connection.ptr()](StringView resource_name) {
                connections_awaiting_opening_handshake().remove_all_matching([&](auto const& other) { return other.ptr() == connection; });
                return accept_connection(*connection, resource_name);
            };
            connection->on_close = [connection = connection.ptr()] {
                connections_awaiting_opening_handshake().remove_all_matching([&](auto const& other) { return other.ptr() == connection; });
            };
            connections_awaiting_opening_handshake().append(move(connection));
        };
    }

    // 2. Let resource name be the result of construct a WebSocket resource name with session.
    // 3. Append resource name to the list of WebSocket resources for listener.
    // 4. Append listener to the remote end's active listeners.
    // NB: The listener accepts a connection for the resource name of any active BiDi session.

    // https://w3c.github.io/webdriver-bidi/#construct-a-websocket-url
    // 1. Let resource name be the result of construct a WebSocket resource name with session.
    // 2. Return a WebSocket URI constructed with host set to listener's host, port set to listener's port, path set to
    //    resource name, following the wss-URI construct if listener's secure flag is set and the ws-URL construct
    //    otherwise.
    return String::formatted("ws://127.0.0.1:{}/session/{}", websocket_listener()->port(), session.session_id());
}

// https://w3c.github.io/webdriver-bidi/#establishing
bool BiDiSession::accept_connection(WebSocketConnection& connection, StringView resource_name)
{
    // 1. Let resource name be the resource name from reading the client's opening handshake. If resource name is not in
    //    listener's list of WebSocket resources, then stop running these steps and act as if the requested service is not
    //    available.
    // 2. If resource name is the byte string "/session", and the implementation supports BiDi-only sessions:
    // NB: We do not support BiDi-only sessions.

    // 3. Get a session ID for a WebSocket resource with resource name and let session id be that value. If session id is
    //    null then stop running these steps and act as if the requested service is not available.
    auto session_id = get_a_session_id_for_a_websocket_resource(resource_name);
    if (!session_id.has_value())
        return false;

    // 4. If there is a session in the list of active sessions with session id as its session ID then let session be
    //    that session. Otherwise stop running these steps and act as if the requested service is not available.
    auto session = Session::find_session(*session_id, Web::WebDriver::SessionFlags::Default, Session::AllowInvalidWindowHandle::Yes);
    if (session.is_error() || !session.value()->bidi_session())
        return false;

    // 5. Run any other implementation-defined steps to decide if the connection should be accepted, and if it is not stop
    //    running these steps and act as if the requested service is not available.
    // NB: The WebSocket layer only accepts connections from the local machine that are not made by web content. A
    //     session also accepts a single connection, so that once the WebDriver client is connected, nothing else can
    //     connect to its session.
    if (!session.value()->bidi_session()->m_session_websocket_connections.is_empty())
        return false;

    // 6. Otherwise append connection to session's session WebSocket connections, and proceed with the WebSocket
    //    server-side requirements when a server chooses to accept an incoming connection.
    session.value()->bidi_session()->add_connection(connection);
    return true;
}

void BiDiSession::add_connection(NonnullRefPtr<WebSocketConnection> connection)
{
    connection->on_message = [this, connection = connection.ptr()](WebSocketConnection::MessageType type, ByteBuffer data) {
        // When a WebSocket message has been received for a WebSocket connection connection with type type and data
        // data, a remote end must handle an incoming message given connection, type and data.
        handle_an_incoming_message(*connection, type, move(data));
    };
    connection->on_close = [this, connection = connection.ptr()] {
        // When the WebSocket closing handshake is started or when the WebSocket connection is closed for a WebSocket
        // connection connection, a remote end must handle a connection closing given connection.
        handle_a_connection_closing(*connection);
    };
    m_session_websocket_connections.append(move(connection));
}

// https://w3c.github.io/webdriver-bidi/#handle-a-connection-closing
void BiDiSession::handle_a_connection_closing(WebSocketConnection& connection)
{
    // 1. If there is a BiDi session associated with connection connection:
    //     1. Let session be the BiDi session associated with connection connection.
    //     2. Remove connection from session's session WebSocket connections.
    m_session_websocket_connections.remove_all_matching([&](auto const& other) { return other.ptr() == &connection; });

    // NB: This does not end any session.
}

// https://w3c.github.io/webdriver-bidi/#close-the-websocket-connections
void BiDiSession::close_the_websocket_connections()
{
    // 1. For each connection in session's session WebSocket connections:
    auto connections = move(m_session_websocket_connections);
    for (auto& connection : connections) {
        // 1. Start the WebSocket closing handshake with connection.
        connection->on_message = nullptr;
        connection->on_close = [connection = connection.ptr()] {
            closing_connections().remove_all_matching([&](auto const& other) { return other.ptr() == connection; });
        };
        connection->start_the_closing_handshake();
        closing_connections().append(connection);
    }
}

// https://w3c.github.io/webdriver-bidi/#handle-an-incoming-message
void BiDiSession::handle_an_incoming_message(WebSocketConnection& connection, WebSocketConnection::MessageType type, ByteBuffer data)
{
    // 1. If type is not text, send an error response given connection, null, and invalid argument, and finally return.
    if (type != WebSocketConnection::MessageType::Text) {
        send_an_error_response(connection, {}, invalid_argument("Messages must be text"sv));
        return;
    }

    // 2. Assert: data is a scalar value string, because the WebSocket handling errors in UTF-8-encoded data would already
    //    have failed the WebSocket connection otherwise.

    // 3. If there is a BiDi Session associated with connection connection, let session be that session. Otherwise if
    //    connection is in WebSocket connections not associated with a session, let session be null. Otherwise, return.
    // NB: Every connection we handle messages for is associated with this session.

    // 4. Let parsed be the result of parsing JSON into Infra values given data. If this throws an exception, then send
    //    an error response given connection, null, and invalid argument, and finally return.
    auto parsed = JsonValue::from_string(StringView { data.bytes() });
    if (parsed.is_error()) {
        send_an_error_response(connection, {}, invalid_argument("Message is not valid JSON"sv));
        return;
    }

    // 5. If session is not null and not in active sessions then return.
    // NB: A session that session.end ended is no longer active, even though it is cleaned up after the response.
    if (!m_session.is_active() || m_has_ended)
        return;

    // 6. Match parsed against the remote end definition. If this results in a match:
    //    Command = { id: js-uint, CommandData, Extensible }
    //    NB: Each command's remote end steps check its params against its own production.
    Optional<JsonValue const&> id;
    Optional<String> method;
    Optional<JsonValue const&> params;
    if (parsed.value().is_object()) {
        auto const& command = parsed.value().as_object();
        id = command.get("id"sv);
        if (auto method_name = command.get_string("method"sv); method_name.has_value())
            method = *method_name;
        params = command.get("params"sv);
    }

    bool matches = id.has_value() && Web::WebDriver::as_js_uint(*id).has_value()
        && method.has_value() && any_of(s_command_names, [&](auto name) { return name == *method; })
        && params.has_value() && params->is_object();

    if (matches) {
        // 1. Let matched be the map representing the matched data.
        // 2. Assert: matched contains "id", "method", and "params".
        // 3. Let command id be matched["id"].
        auto command_id = *id;

        // 4. Let method be matched["method"]
        // 5. Let command be the command with command name method.
        // 6. If session is null and command is not a static command, then send an error response given connection,
        //    command id, and invalid session id, and return.
        // NB: Session is never null.

        // 7. Run the following steps in parallel:
        auto promise = run_command(*method, *params);
        promise->when_resolved([this, connection = NonnullRefPtr { connection }, command_id, method = *method](JsonValue& value) mutable {
                   // 3. Let value be result's data.
                   // 4. Assert: value matches the definition for the result type corresponding to the command with
                   //    command name method.
                   // NB: A command whose result type is EmptyResult completes with null.
                   if (value.is_null())
                       value = JsonObject {};

                   // 5. If method is "session.new", let session be the entry in the list of active sessions whose session ID
                   //    is equal to the "sessionId" property of value, append connection to session's session WebSocket
                   //    connections, and remove connection from the WebSocket connections not associated with a session.
                   // 6. Let response be a new map matching the CommandResponse production in the local end definition with
                   //    the id field set to command id and the value field set to value.
                   // 7. Let serialized be the result of serialize an infra value to JSON bytes given response.
                   // 8. Send a WebSocket message comprised of serialized over connection.
                   send_a_success_response(*connection, move(command_id), move(value));

                   // https://w3c.github.io/webdriver-bidi/#command-session-end
                   // 2. Return success with data null, and in parallel run the following steps:
                   //     1. Wait until the Send a WebSocket message steps have been called with the response to this command.
                   //     2. Cleanup the session with session.
                   if (method == "session.end"sv) {
                       Core::deferred_invoke([session = NonnullRefPtr { m_session }] {
                           session->close();
                       });
                   }
               })
            .when_rejected([this, connection = NonnullRefPtr { connection }, command_id](Web::WebDriver::Error& error) mutable {
                // 2. If result is an error, then send an error response given connection, command id, and result's
                //    error code, and finally return.
                send_an_error_response(*connection, move(command_id), error);
            });
        return;
    }

    // 7. Otherwise:
    // 1. Let command id be null.
    JsonValue command_id;

    // 2. If parsed is a map and parsed["id"] exists and is an integer greater than or equal to zero, set command id to
    //    that integer.
    if (id.has_value() && Web::WebDriver::as_js_uint(*id).has_value())
        command_id = *id;

    // 3. Let error code be invalid argument.
    auto error = invalid_argument("Message does not match the format of a command"sv);

    // 4. If parsed is a map and parsed["method"] exists and is a string, but parsed["method"] is not in the set of all
    //    command names, set error code to unknown command.
    if (method.has_value() && !any_of(s_command_names, [&](auto name) { return name == *method; }))
        error = Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownCommand, "Unknown command"sv);

    // 5. Send an error response given connection, command id, and error code.
    send_an_error_response(connection, move(command_id), error);
}

NonnullRefPtr<BiDiSession::WebDriverPromise> BiDiSession::run_command(StringView method, JsonValue params)
{
    if (method == "session.status"sv)
        return session_status();
    if (method == "session.new"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-session-new
        // 1. If session is not null, return an error with error code session not created.
        return WebDriverPromise::rejected(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, "A session already exists"sv));
    }
    if (method == "session.end"sv)
        return session_end();
    if (method == "session.subscribe"sv)
        return session_subscribe(move(params));
    if (method == "session.unsubscribe"sv)
        return session_unsubscribe(move(params));

    // NB: The browser process runs every command of the other modules.
    return m_session.perform_bidi_browser_command(method, move(params));
}

// https://w3c.github.io/webdriver-bidi/#send-an-error-response
void BiDiSession::send_an_error_response(WebSocketConnection& connection, JsonValue command_id, Web::WebDriver::Error const& error)
{
    // 1. Let error data be a new map matching the ErrorResponse production in the local end definition, with the id field
    //    set to command id, the error field set to error code, the message field set to an implementation-defined string
    //    containing a human-readable definition of the error that occurred and the stacktrace field optionally set to an
    //    implementation-defined string containing a stack trace report of the active stack frames at the time when the
    //    error occurred.
    JsonObject error_data;
    error_data.set("type"sv, "error"sv);
    error_data.set("id"sv, move(command_id));
    error_data.set("error"sv, error.error);
    error_data.set("message"sv, error.message);

    // 2. Let response be the result of serialize an infra value to JSON bytes given error data.
    auto response = error_data.serialized();

    // 3. Send a WebSocket message comprised of response over connection.
    connection.send_text_message(response);
}

void BiDiSession::send_a_success_response(WebSocketConnection& connection, JsonValue command_id, JsonValue result)
{
    JsonObject response;
    response.set("type"sv, "success"sv);
    response.set("id"sv, move(command_id));
    response.set("result"sv, move(result));

    connection.send_text_message(response.serialized());
}

// https://w3c.github.io/webdriver-bidi/#emit-an-event
void BiDiSession::emit_an_event(StringView name, JsonValue params)
{
    // 1. Assert: body matches the Event production.
    JsonObject body;
    body.set("type"sv, "event"sv);
    body.set("method"sv, name);
    body.set("params"sv, move(params));

    // 2. Let serialized be the result of serialize an infra value to JSON bytes given body.
    auto serialized = body.serialized();

    // 3. For each connection in session's session WebSocket connections:
    for (auto& connection : m_session_websocket_connections) {
        // 1. Send a WebSocket message comprised of serialized over connection.
        connection->send_text_message(serialized);
    }
}

// https://w3c.github.io/webdriver-bidi/#event-is-enabled
// NB: The browser process gives the top-level traversables and user contexts of the navigables an event relates to.
bool BiDiSession::event_is_enabled(StringView event_name, Vector<String> const& top_level_traversable_ids, Vector<String> const& user_context_ids) const
{
    // 1. Let top-level traversables be get top-level traversables with navigables.

    // 2. For each subscription in session's subscriptions:
    for (auto const& subscription : m_subscriptions) {
        // 1. If subscription's event names do not contains event name, continue.
        if (!subscription.event_names.contains(event_name))
            continue;

        // 2. If subscription is global return true.
        if (subscription.is_global())
            return true;

        // 3. If user context ids is not empty:
        if (!subscription.user_context_ids.is_empty()) {
            // 1. For each navigable in top-level traversables:
            //     1. If subscription's user context ids contains navigable's associated user context's user context id,
            //        return true.
            for (auto const& user_context_id : user_context_ids) {
                if (subscription.user_context_ids.contains(user_context_id))
                    return true;
            }
        }
        // 4. Otherwise:
        else {
            // 1. Let subscription top-level traversables be get navigables by ids with subscription's top-level
            //    traversable ids.
            // 2. If the intersection of top-level traversables and subscription top-level traversables is not empty
            //    return true.
            for (auto const& top_level_traversable_id : top_level_traversable_ids) {
                if (subscription.top_level_traversable_ids.contains(top_level_traversable_id))
                    return true;
            }
        }
    }

    // 3. Return false.
    return false;
}

// https://w3c.github.io/webdriver-bidi/#set-of-top-level-traversables-for-which-an-event-is-enabled
HashTable<String> BiDiSession::set_of_top_level_traversables_for_which_an_event_is_enabled(StringView event_name) const
{
    // 1. Let result be a new set.
    HashTable<String> result;

    // 2. For each subscription in session's subscriptions:
    for (auto const& subscription : m_subscriptions) {
        // 1. If subscription's event names does not contain event name, continue.
        if (!subscription.event_names.contains(event_name))
            continue;

        // 2. If subscription's is global:
        if (subscription.is_global()) {
            // 1. For each traversable in remote end's top-level traversables:
            //     1. Append traversable to result.
            for (auto const& traversable : m_session.top_level_traversable_ids())
                result.set(traversable);

            // 2. Break.
            break;
        }

        // 3. Otherwise, if user context ids is not empty:
        if (!subscription.user_context_ids.is_empty()) {
            // 1. For each traversable in remote end's top-level traversables:
            //     1. Append traversable to result if subscription's user context ids contains traversable's associated
            //        user context's user context id.
            if (subscription.user_context_ids.contains(DEFAULT_USER_CONTEXT_ID)) {
                for (auto const& traversable : m_session.top_level_traversable_ids())
                    result.set(traversable);
            }
        }
        // 4. Otherwise:
        else {
            // 1. Let top-level traversables be get navigables by ids with subscription's top-level traversable ids.
            // 2. Append each item of top-level traversables to result.
            for (auto const& traversable : subscription.top_level_traversable_ids) {
                if (m_session.top_level_traversable_ids().contains_slow(traversable))
                    result.set(traversable);
            }
        }
    }

    // 3. Return result.
    return result;
}

// NB: The browser process only reports the events some subscription enables.
void BiDiSession::update_enabled_events()
{
    HashTable<String> enabled_events;
    for (auto const& subscription : m_subscriptions) {
        for (auto const& event_name : subscription.event_names)
            enabled_events.set(event_name);
    }

    if (sets_are_equal(enabled_events, m_enabled_events))
        return;
    m_enabled_events = move(enabled_events);

    Vector<String> event_names;
    for (auto const& event_name : m_enabled_events)
        event_names.append(event_name);
    m_session.set_bidi_enabled_events(move(event_names));
}

void BiDiSession::did_receive_event(String const& name, JsonValue params, Vector<String> const& top_level_traversable_ids, Vector<String> const& user_context_ids)
{
    // NB: The browser process names the events defined by this specification, whose parameters are maps.
    if (!any_of(s_event_names, [&](auto event_name) { return event_name == name; }) || !params.is_object())
        return;

    Optional<String> destroyed_navigable_id;
    if (name == "browsingContext.contextDestroyed"sv) {
        if (auto context = params.as_object().get_string("context"sv); context.has_value())
            destroyed_navigable_id = *context;
    }
    ScopeGuard remove_destroyed_navigable = [&] {
        // https://w3c.github.io/webdriver-bidi/#event-browsingContext-contextDestroyed
        // NB: This runs for the session whether or not it is subscribed to the event.
        if (destroyed_navigable_id.has_value())
            remove_destroyed_navigable_from_subscriptions(*destroyed_navigable_id);
    };

    // For each session in the set of sessions for which an event is enabled given event name and related navigables:
    if (!event_is_enabled(name, top_level_traversable_ids, user_context_ids)) {
        // https://w3c.github.io/webdriver-bidi/#event-log-entryAdded
        // Otherwise, buffer a log event with session, related browsing contexts, and body.
        if (name == "log.entryAdded"sv)
            buffer_a_log_event(top_level_traversable_ids, move(params));
        return;
    }

    // Emit an event with session and body.
    emit_an_event(name, move(params));
}

void BiDiSession::did_close_top_level_traversable(String const& id)
{
    // NB: Events of a closed top-level traversable that are buffered are never emitted.
    m_log_event_buffer.remove(id);

    // NB: The browser process reports browsingContext.contextDestroyed only while an event of that name is enabled,
    //     and that event removes the navigable from the subscriptions then.
    if (!m_enabled_events.contains("browsingContext.contextDestroyed"sv))
        remove_destroyed_navigable_from_subscriptions(id);
}

// https://w3c.github.io/webdriver-bidi/#buffer-a-log-event
// NB: The buffer is keyed by the top-level traversables of the navigables the event relates to, whose ids WebDriver
//     knows. The specification leaves open whether to key it on browsing contexts or top-level traversables.
void BiDiSession::buffer_a_log_event(Vector<String> const& top_level_traversable_ids, JsonValue event)
{
    // 1. Let buffer be session's log event buffer.
    // 2. Let navigable ids be a new list.
    // 3. For each navigable of navigables:
    //     1. Append the navigable id for navigable to navigable ids.
    // 4. For each navigable id in navigable ids:
    // NB: The other navigables an event relates to are only relevant for workers, which do not report log events yet.
    auto event_size = event.serialized().byte_count();
    for (auto const& navigable_id : top_level_traversable_ids) {
        // 1. Let other navigables be an empty list
        // 2. For each other id in navigable ids:
        //     1. If other id is not equal to navigable id, append other id to other navigables.
        // 3. If buffer does not contain navigable id, let buffer[navigable id] be a new list.
        auto& buffer = m_log_event_buffer.ensure(navigable_id);

        // 4. Append (event, other navigables) to buffer[navigable id].
        if (event_size > MAXIMUM_LOG_EVENT_BUFFER_BYTES)
            continue;
        while (!buffer.events.is_empty() && (buffer.events.size() >= MAXIMUM_LOG_EVENT_BUFFER_SIZE || buffer.size + event_size > MAXIMUM_LOG_EVENT_BUFFER_BYTES))
            buffer.size -= buffer.events.take_first().size;
        buffer.events.append(BufferedLogEvent { .event = event, .size = event_size });
        buffer.size += event_size;
    }
}

// https://w3c.github.io/webdriver-bidi/#event-log-entryAdded
void BiDiSession::run_log_entry_added_subscribe_steps(HashTable<String> const& navigables, bool include_global)
{
    // 1. For each navigable id → events in session's log event buffer:
    Vector<String> navigable_ids_to_remove;
    auto top_level_traversable_ids = m_session.top_level_traversable_ids();
    for (auto& [navigable_id, buffer] : m_log_event_buffer) {
        // 1. Let maybe context be the result of getting a navigable given navigable id.
        // 2. If maybe context is an error, remove navigable id from log event buffer and continue.
        if (!top_level_traversable_ids.contains_slow(navigable_id)) {
            navigable_ids_to_remove.append(navigable_id);
            continue;
        }

        // 3. Let navigable be maybe context's data
        // 4. Let top level navigable be navigable's top-level traversable.
        // 5. If include global is true and top level navigable is not in navigables, or if include global is false and
        //    top level navigable is in navigables:
        // AD-HOC: This emits the events of the top-level traversables the subscription newly enables, which local ends
        //         expect, whether or not the subscription is global.
        (void)include_global;
        if (!navigables.contains(navigable_id))
            continue;

        // 1. For each (event, other navigables) in events:
        for (auto& buffered_event : buffer.events) {
            // 1. Emit an event with session and event.
            emit_an_event("log.entryAdded"sv, move(buffered_event.event));

            // 2. For each other context id in other navigables:
            //     1. If log event buffer contains other context id, remove event from log event buffer[other context id].
        }

        // NB: An emitted event is not emitted again.
        navigable_ids_to_remove.append(navigable_id);
    }

    for (auto const& navigable_id : navigable_ids_to_remove)
        m_log_event_buffer.remove(navigable_id);
}

// https://w3c.github.io/webdriver-bidi/#event-browsingContext-contextDestroyed
void BiDiSession::remove_destroyed_navigable_from_subscriptions(String const& id)
{
    // 3. Let subscriptions to remove be a set.
    // 4. For each subscription in session's subscriptions:
    //     1. If subscription's top-level traversable ids contains navigable's navigable id;
    //         1. Remove navigable's navigable id from subscription's top-level traversable ids.
    //         2. If subscription's top-level traversable ids is empty:
    //             1. Append subscription to subscriptions to remove.
    // 5. Remove subscriptions to remove from session's subscriptions.
    m_subscriptions.remove_all_matching([&](auto& subscription) {
        if (!subscription.top_level_traversable_ids.remove(id))
            return false;
        return subscription.top_level_traversable_ids.is_empty();
    });
    update_enabled_events();
}

// https://w3c.github.io/webdriver-bidi/#readiness-state
static bool readiness_state()
{
    return Session::session_count(Web::WebDriver::SessionFlags::Http) == 0 && !Session::has_pending_http_session_creation();
}

// https://w3c.github.io/webdriver-bidi/#command-session-status
NonnullRefPtr<BiDiSession::WebDriverPromise> BiDiSession::session_status()
{
    auto ready = readiness_state();

    // 1. Let body be a new map with the following properties:
    //    "ready"
    //        The remote end’s readiness state.
    //    "message"
    //        An implementation-defined string explaining the remote end’s readiness state.
    JsonObject body;
    body.set("ready"sv, ready);
    body.set("message"sv, ready ? "Ready to accept a new session"sv : "Not ready to accept a new session"sv);

    // 2. Return success with data body
    return WebDriverPromise::resolved(JsonValue { move(body) });
}

// https://w3c.github.io/webdriver-bidi/#command-session-end
NonnullRefPtr<BiDiSession::WebDriverPromise> BiDiSession::session_end()
{
    // 1. End the session with session.
    m_has_ended = true;

    // 2. Return success with data null, and in parallel run the following steps:
    //     1. Wait until the Send a WebSocket message steps have been called with the response to this command.
    //     2. Cleanup the session with session.
    // NB: The session closes once the response is sent.
    return WebDriverPromise::resolved(JsonValue {});
}

static ErrorOr<Vector<String>, Web::WebDriver::Error> get_non_empty_list_of_strings(JsonObject const& params, StringView name)
{
    auto value = params.get(name);
    if (!value.has_value() || !value->is_array() || value->as_array().is_empty())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a non-empty list of strings", name)));

    Vector<String> result;
    for (auto const& item : value->as_array().values()) {
        if (!item.is_string())
            return invalid_argument(MUST(String::formatted("Parameter '{}' must be a non-empty list of strings", name)));
        result.append(item.as_string());
    }
    return result;
}

// https://w3c.github.io/webdriver-bidi/#command-session-subscribe
NonnullRefPtr<BiDiSession::WebDriverPromise> BiDiSession::session_subscribe(JsonValue params_value)
{
    // session.SubscribeParameters = {
    //   events: [+text],
    //   ? contexts: [+browsingContext.BrowsingContext],
    //   ? userContexts: [+browser.UserContext],
    // }
    auto const& params = params_value.as_object();
    auto events = TRY_OR_REJECT(get_non_empty_list_of_strings(params, "events"sv));

    Vector<String> contexts;
    if (params.has("contexts"sv))
        contexts = TRY_OR_REJECT(get_non_empty_list_of_strings(params, "contexts"sv));

    Vector<String> user_contexts;
    if (params.has("userContexts"sv))
        user_contexts = TRY_OR_REJECT(get_non_empty_list_of_strings(params, "userContexts"sv));

    // 1. Let event names be an empty set.
    HashTable<String> event_names;

    // 2. For each entry name in command parameters["events"], let event names be the union of event names and the
    //    result of trying to obtain a set of event names with name.
    for (auto const& name : events) {
        auto names = TRY_OR_REJECT(obtain_a_set_of_event_names(name));
        for (auto& event_name : names)
            event_names.set(move(event_name));
    }

    // 3. Let input user context ids be create a set with command parameters[userContexts].
    HashTable<String> input_user_context_ids;
    for (auto const& user_context : user_contexts)
        input_user_context_ids.set(user_context);

    // 4. Let input context ids be create a set with command parameters[contexts].
    HashTable<String> input_context_ids;
    for (auto const& context : contexts)
        input_context_ids.set(context);

    // 5. If input user context ids is not empty and input context ids is not empty, return error with error code
    //    invalid argument.
    if (!input_user_context_ids.is_empty() && !input_context_ids.is_empty())
        return WebDriverPromise::rejected(invalid_argument("Parameters 'contexts' and 'userContexts' are mutually exclusive"sv));

    // 6. Let subscription navigables be a set.
    HashTable<String> subscription_navigables;

    // 7. Let top-level traversable context ids be a set.
    HashTable<String> top_level_traversable_context_ids;

    // 8. If input context ids is not empty:
    if (!input_context_ids.is_empty()) {
        // 1. Let navigables be the result of trying to get valid navigables by ids with input context ids.
        // 2. Set subscription navigables be get top-level traversables with navigables.
        // 3. For each navigable in subscription navigables:
        //     1. Append navigable's navigable id to top-level traversable context ids.
        // NB: The browser process holds the navigables.
        auto promise = WebDriverPromise::construct();
        auto top_level_traversables_promise = m_session.get_bidi_top_level_traversables(move(contexts));
        promise->add_child(top_level_traversables_promise);

        top_level_traversables_promise->when_resolved([this, promise, event_names = move(event_names), input_user_context_ids = move(input_user_context_ids)](JsonValue& top_level_traversables) mutable {
                                          if (!top_level_traversables.is_array() || !all_of(top_level_traversables.as_array().values(), [](auto const& id) { return id.is_string(); })) {
                                              promise->reject(invalid_browser_response());
                                              return;
                                          }

                                          HashTable<String> subscription_navigables;
                                          for (auto const& id : top_level_traversables.as_array().values())
                                              subscription_navigables.set(id.as_string());

                                          auto top_level_traversable_context_ids = subscription_navigables;
                                          auto subscribe_promise = complete_subscribe(move(event_names), move(subscription_navigables), move(top_level_traversable_context_ids), move(input_user_context_ids));
                                          promise->add_child(subscribe_promise);
                                          subscribe_promise->when_resolved([promise](JsonValue& value) { promise->resolve(move(value)); })
                                              .when_rejected([promise](auto& error) { promise->reject(move(error)); });
                                      })
            .when_rejected([promise](auto& error) {
                promise->reject(move(error));
            });
        return promise;
    }

    // 9. Otherwise, if input user context ids is not empty:
    if (!input_user_context_ids.is_empty()) {
        // 1. For each user context id of input user context ids:
        for (auto const& user_context_id : input_user_context_ids) {
            // 1. Let user context be get user context with user context id.
            // 2. If user context is null, return error with error code no such user context.
            if (user_context_id != DEFAULT_USER_CONTEXT_ID)
                return WebDriverPromise::rejected(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchUserContext, "User context not found"sv));

            // 3. For each top-level traversable in the list of all top-level traversables whose associated user context
            //    is user context:
            //     1. Append top-level traversable to subscription navigables.
            for (auto const& traversable : m_session.top_level_traversable_ids())
                subscription_navigables.set(traversable);
        }
    }
    // 10. Otherwise, set subscription navigables to a set of all top-level traversables in the remote end.
    else {
        for (auto const& traversable : m_session.top_level_traversable_ids())
            subscription_navigables.set(traversable);
    }

    return complete_subscribe(move(event_names), move(subscription_navigables), move(top_level_traversable_context_ids), move(input_user_context_ids));
}

NonnullRefPtr<BiDiSession::WebDriverPromise> BiDiSession::complete_subscribe(HashTable<String> event_names, HashTable<String> subscription_navigables, HashTable<String> top_level_traversable_context_ids, HashTable<String> input_user_context_ids)
{
    // 11. Let subscription be a subscription with subscription id set to the string representation of a UUID, event
    //     names set to event names, top-level traversable ids set to top-level traversable context ids and user context
    //     ids set to input user context ids.
    Subscription subscription {
        .subscription_id = generate_random_uuid(),
        .event_names = move(event_names),
        .top_level_traversable_ids = move(top_level_traversable_context_ids),
        .user_context_ids = move(input_user_context_ids),
    };

    // 12. Let subscribe step events be a new map.
    HashMap<String, HashTable<String>> subscribe_step_events;

    // 13. For each event name in the event names:
    for (auto const& event_name : subscription.event_names) {
        // 1. If the event with event name event name does not define remote end subscribe steps, continue;
        if (!subscribe_priority(event_name).has_value())
            continue;

        // 2. Let existing navigables be a set of top-level traversables for which an event is enabled with session and
        //    event name.
        auto existing_navigables = set_of_top_level_traversables_for_which_an_event_is_enabled(event_name);

        // 3. Set subscribe step events[event name] to difference of subscription navigables and existing navigables.
        HashTable<String> navigables;
        for (auto const& navigable : subscription_navigables) {
            if (!existing_navigables.contains(navigable))
                navigables.set(navigable);
        }
        subscribe_step_events.set(event_name, move(navigables));
    }

    // 16. Sort in ascending order subscribe step events using the following less than algorithm given two entries with
    //     keys event name one and event name two:
    //     1. Let event one be the event with name event name one
    //     2. Let event two be the event with name event name two
    //     3. Return true if event one's subscribe priority is less than event two subscribe priority, or false otherwise.
    Vector<String> sorted_event_names;
    for (auto const& entry : subscribe_step_events)
        sorted_event_names.append(entry.key);
    quick_sort(sorted_event_names, [](auto const& one, auto const& two) {
        return *subscribe_priority(one) < *subscribe_priority(two);
    });

    // 17. If subscription is global, let include global be true, otherwise let include global be false.
    auto include_global = subscription.is_global();

    // 19. Let body be a new map matching the session.SubscribeResult production, with the subscription field set to
    //     subscription's subscription id.
    JsonObject body;
    body.set("subscription"sv, subscription.subscription_id);

    // 14. Append subscription to session's subscriptions.
    // 15. Append subscription's subscription id to session's known subscription ids.
    m_known_subscription_ids.set(subscription.subscription_id);
    m_subscriptions.append(move(subscription));
    update_enabled_events();

    // 18. For each event name → navigables in subscribe step events:
    //     1. Run the remote end subscribe steps for the event with event name event name given session, navigables and
    //        include global.
    auto promise = WebDriverPromise::construct();
    run_subscribe_steps(move(sorted_event_names), move(subscribe_step_events), include_global, promise, move(body));

    // 20. Return success with data body.
    return promise;
}

void BiDiSession::run_subscribe_steps(Vector<String> event_names, HashMap<String, HashTable<String>> subscribe_step_events, bool include_global, NonnullRefPtr<WebDriverPromise> promise, JsonValue body)
{
    if (event_names.is_empty()) {
        promise->resolve(move(body));
        return;
    }

    auto event_name = event_names.take_first();
    auto navigables = subscribe_step_events.take(event_name).release_value();

    // NB: WebDriver holds the log event buffer.
    if (event_name == "log.entryAdded"sv) {
        run_log_entry_added_subscribe_steps(navigables, include_global);
        run_subscribe_steps(move(event_names), move(subscribe_step_events), include_global, move(promise), move(body));
        return;
    }

    Vector<String> navigable_ids;
    for (auto const& navigable : navigables)
        navigable_ids.append(navigable);

    // NB: The browser process runs the subscribe steps, and gives the events they emit.
    auto steps_promise = m_session.run_bidi_subscribe_steps(event_name, move(navigable_ids));
    promise->add_child(steps_promise);

    steps_promise->when_resolved([this, event_names = move(event_names), subscribe_step_events = move(subscribe_step_events), include_global, promise, body = move(body)](JsonValue& events) mutable {
                     auto is_valid_event = [&](JsonValue const& event) {
                         if (!event.is_object())
                             return false;
                         auto method = event.as_object().get_string("method"sv);
                         auto params = event.as_object().get("params"sv);
                         return method.has_value() && any_of(s_event_names, [&](auto name) { return name == *method; }) && params.has_value() && params->is_object();
                     };
                     if (!events.is_array() || !all_of(events.as_array().values(), is_valid_event)) {
                         promise->reject(invalid_browser_response());
                         return;
                     }

                     for (auto& event : events.as_array().values()) {
                         auto const& event_object = event.as_object();
                         emit_an_event(*event_object.get_string("method"sv), *event_object.get("params"sv));
                     }
                     run_subscribe_steps(move(event_names), move(subscribe_step_events), include_global, promise, move(body));
                 })
        .when_rejected([promise](auto& error) {
            promise->reject(move(error));
        });
}

// https://w3c.github.io/webdriver-bidi/#command-session-unsubscribe
NonnullRefPtr<BiDiSession::WebDriverPromise> BiDiSession::session_unsubscribe(JsonValue params_value)
{
    // session.UnsubscribeParameters = session.UnsubscribeByAttributesRequest / session.UnsubscribeByIDRequest
    auto const& params = params_value.as_object();

    // 1. If command parameters does not contain "subscriptions":
    if (!params.has("subscriptions"sv)) {
        // NB: The condition implies that command parameters is matching the session.UnsubscribeByAttributesRequest
        //     production.
        auto events = TRY_OR_REJECT(get_non_empty_list_of_strings(params, "events"sv));

        // 1. Let event names be an empty set.
        HashTable<String> event_names;

        // 2. For each entry name in command parameters["events"], let event names be the union of event names and the
        //    result of trying to obtain a set of event names with name.
        for (auto const& name : events) {
            auto names = TRY_OR_REJECT(obtain_a_set_of_event_names(name));
            for (auto& event_name : names)
                event_names.set(move(event_name));
        }

        // 3. Let new subscriptions to be a list.
        Vector<Subscription> new_subscriptions;

        // 4. Let matched events to be a set.
        HashTable<String> matched_events;

        // 5. For each subscription of session's subscriptions:
        for (auto const& subscription : m_subscriptions) {
            // 1. If intersection of subscription's event names and event names is an empty set:
            if (!any_of(event_names, [&](auto const& event_name) { return subscription.event_names.contains(event_name); })) {
                // 1. append subscription to new subscriptions.
                new_subscriptions.append(subscription);

                // 2. Continue.
                continue;
            }

            // 2. If subscription is not global:
            if (!subscription.is_global()) {
                // 1. append subscription to new subscriptions.
                new_subscriptions.append(subscription);

                // 2. Continue.
                continue;
            }

            // 3. Let subscription event names be clone of subscription's event names.
            auto subscription_event_names = subscription.event_names;

            // 4. For each event name of event names:
            for (auto const& event_name : event_names) {
                // 1. If subscription event names contains event name:
                if (subscription_event_names.contains(event_name)) {
                    // 1. Append event name to matched events.
                    matched_events.set(event_name);

                    // 2. Remove event name from subscription event names.
                    subscription_event_names.remove(event_name);
                }
            }

            // 5. If subscription event names is not empty:
            if (!subscription_event_names.is_empty()) {
                // 1. Let cloned subscription be a subscription with subscription id set to subscription's subscription id,
                //    event names set to a new set containing subscription event names.
                // 2. append cloned subscription to new subscriptions.
                new_subscriptions.append({
                    .subscription_id = subscription.subscription_id,
                    .event_names = move(subscription_event_names),
                    .top_level_traversable_ids = {},
                    .user_context_ids = {},
                });
            }
        }

        // 6. If matched events is not equal to event names, return error with error code invalid argument.
        if (!sets_are_equal(matched_events, event_names))
            return WebDriverPromise::rejected(invalid_argument("No subscription matches the given events"sv));

        // 7. Set session's subscriptions to new subscriptions.
        m_subscriptions = move(new_subscriptions);
    }
    // 2. Otherwise:
    else {
        auto subscription_ids = TRY_OR_REJECT(get_non_empty_list_of_strings(params, "subscriptions"sv));

        // 1. Let subscriptions be create a set with command parameters[subscriptions].
        HashTable<String> subscriptions;
        for (auto const& subscription_id : subscription_ids)
            subscriptions.set(subscription_id);

        // 2. Let unknown subscription ids to difference between subscriptions and session's known subscription ids.
        // 3. If unknown subscription ids is not empty:
        //     1. Return error with error code invalid argument.
        if (any_of(subscriptions, [&](auto const& id) { return !m_known_subscription_ids.contains(id); }))
            return WebDriverPromise::rejected(invalid_argument("Unknown subscription id"sv));

        // 4. Let subscriptions to remove be an empty set.
        // 5. For each subscription in session's subscriptions:
        //     1. If subscriptions contains subscription's subscription id:
        //         1. Append subscription to subscriptions to remove.
        // 6. Set session's known subscription ids to difference between session's known subscription ids and
        //    subscriptions.
        for (auto const& subscription_id : subscriptions)
            m_known_subscription_ids.remove(subscription_id);

        // 7. Remove each item in subscriptions to remove from session's subscriptions.
        m_subscriptions.remove_all_matching([&](auto const& subscription) {
            return subscriptions.contains(subscription.subscription_id);
        });
    }

    update_enabled_events();

    // 3. Return success with data null.
    return WebDriverPromise::resolved(JsonValue {});
}

}
