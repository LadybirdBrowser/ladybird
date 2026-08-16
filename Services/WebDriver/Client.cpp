/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibURL/Parser.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <WebDriver/Client.h>
#include <WebDriver/Session.h>

namespace WebDriver {

static ErrorOr<NonnullRefPtr<Session>, Web::WebDriver::Error>
find_session_with_ladybird_test_hooks(Web::WebDriver::Parameters const& parameters)
{
    auto session = TRY(Session::find_session(parameters[0]));
    if (!session->test_hooks_enabled())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownCommand,
            "Ladybird test hooks are not enabled for this session"sv);
    return session;
}

ErrorOr<NonnullRefPtr<Client>> Client::try_create(NonnullOwnPtr<Core::BufferedTCPSocket> socket, LaunchBrowserCallback launch_browser_callback)
{
    if (!launch_browser_callback)
        return Error::from_string_literal("The callback to launch the browser must be provided");

    TRY(socket->set_blocking(true));
    return adopt_nonnull_ref_or_enomem(new (nothrow) Client(move(socket), move(launch_browser_callback)));
}

Client::Client(NonnullOwnPtr<Core::BufferedTCPSocket> socket, LaunchBrowserCallback launch_browser_callback)
    : Web::WebDriver::Client(move(socket))
    , m_launch_browser_callback(move(launch_browser_callback))
{
}

Client::~Client() = default;

// 8.1 New Session, https://w3c.github.io/webdriver/#dfn-new-sessions
// POST /session
Web::WebDriver::Response Client::new_session(Web::WebDriver::Parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session");

    // 1. If the implementation is an endpoint node, and the list of active HTTP sessions is not empty, or otherwise if
    //    the implementation is unable to start an additional session, return error with error code session not created.
    if (Session::session_count(Web::WebDriver::SessionFlags::Http) > 0)
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, "There is already an active HTTP session"sv);

    // FIXME: 2. If the remote end is an intermediary node, take implementation-defined steps that either result in returning
    //           an error with error code session not created, or in returning a success with data that is isomorphic to that
    //           returned by remote ends according to the rest of this algorithm. If an error is not returned, the intermediary
    //           node must retain a reference to the session created on the upstream node as the associated session such that
    //           commands may be forwarded to this associated session on subsequent commands.

    // 3. Let flags be a set containing "http".
    static constexpr auto flags = Web::WebDriver::SessionFlags::Http;

    // 4. Let capabilities be the result of trying to process capabilities with parameters and flags.
    auto capabilities = TRY(Web::WebDriver::process_capabilities(payload, flags));

    // 5. If capabilities's is null, return error with error code session not created.
    if (capabilities.is_null())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, "Could not match capabilities"sv);

    // 6. Let session be the result of create a session, with capabilities, and flags.
    auto maybe_session = Session::create(*this, capabilities.as_object(), flags);
    if (maybe_session.is_error())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, MUST(String::formatted("Failed to start session: {}", maybe_session.error())));

    auto session = maybe_session.release_value();

    // 7. Let body be a JSON Object initialized with:
    JsonObject body;
    // "sessionId"
    //     session's session ID.
    body.set("sessionId"sv, JsonValue { session->session_id() });
    // "capabilities"
    //     capabilities
    body.set("capabilities"sv, move(capabilities));

    // 8. Set session' current top-level browsing context to one of the endpoint node's top-level browsing contexts,
    //    preferring the top-level browsing context that has system focus, or otherwise preferring any top-level
    //    browsing context whose visibility state is visible.
    // NOTE: This happens in the WebContent process.

    // FIXME: 9. Set the request queue to a new queue.

    // 10. Return success with data body.
    return JsonValue { move(body) };
}

// 8.2 Delete Session, https://w3c.github.io/webdriver/#dfn-delete-session
// DELETE /session/{session id}
Web::WebDriver::Response Client::delete_session(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>");

    // 1. If session is an active HTTP session, try to close the session with session.
    if (auto session = Session::find_session(parameters[0], Web::WebDriver::SessionFlags::Http, Session::AllowInvalidWindowHandle::Yes); !session.is_error())
        session.value()->close();

    // 2. Return success with data null.
    return JsonValue {};
}

// https://w3c.github.io/webdriver/#dfn-readiness-state
static bool readiness_state()
{
    // The readiness state of a remote end indicates whether it is free to accept new connections. It must be false if
    // the implementation is an endpoint node and the list of active HTTP sessions is not empty, or otherwise if the
    // remote end is known to be in a state in which attempting to create new sessions would fail. In all other cases it
    // must be true.
    return Session::session_count(Web::WebDriver::SessionFlags::Http) == 0;
}

// 8.3 Status, https://w3c.github.io/webdriver/#dfn-status
// GET /status
Web::WebDriver::Response Client::get_status(Web::WebDriver::Parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /status");

    auto readiness_state = WebDriver::readiness_state();

    // 1. Let body be a new JSON Object with the following properties:
    //    "ready"
    //        The remote end's readiness state.
    //    "message"
    //        An implementation-defined string explaining the remote end's readiness state.
    JsonObject body;
    body.set("ready"sv, readiness_state);
    body.set("message"sv, MUST(String::formatted("{} to accept a new session", readiness_state ? "Ready"sv : "Not ready"sv)));

    // 2. Return success with data body.
    return JsonValue { body };
}

// 9.1 Get Timeouts, https://w3c.github.io/webdriver/#dfn-get-timeouts
// GET /session/{session id}/timeouts
Web::WebDriver::Response Client::get_timeouts(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session id>/timeouts");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->get_timeouts();
}

// 9.2 Set Timeouts, https://w3c.github.io/webdriver/#dfn-set-timeouts
// POST /session/{session id}/timeouts
Web::WebDriver::Response Client::set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session id>/timeouts");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->set_timeouts(move(payload));
}

// 10.1 Navigate To, https://w3c.github.io/webdriver/#dfn-navigate-to
// POST /session/{session id}/url
Web::WebDriver::Response Client::navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/url");
    auto session = TRY(Session::find_session(parameters[0]));

    // 2. Let url be the result of getting the property url from the parameters argument.
    if (!payload.is_object() || !payload.as_object().has_string("url"sv))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload doesn't have a string `url`"sv);
    auto url = URL::Parser::basic_parse(payload.as_object().get_string("url"sv).value());
    if (!url.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload has an invalid `url`"sv);

    // FIXME: 3. If url is not an absolute URL or is not an absolute URL with fragment or not a local scheme, return error with error code invalid argument.

    auto response = TRY(session->navigate_to(url.release_value()));
    response = TRY(session->wait_for_navigation_completion());
    return response;
}

// 10.2 Get Current URL, https://w3c.github.io/webdriver/#dfn-get-current-url
// GET /session/{session id}/url
Web::WebDriver::Response Client::get_current_url(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/url");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_current_url"sv);
}

// 10.3 Back, https://w3c.github.io/webdriver/#dfn-back
// POST /session/{session id}/back
Web::WebDriver::Response Client::back(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/back");
    auto session = TRY(Session::find_session(parameters[0]));

    auto response = TRY(session->traverse_history(-1, Session::HandleUserPrompts::Yes));
    response = TRY(session->wait_for_navigation_completion());
    return response;
}

// 10.4 Forward, https://w3c.github.io/webdriver/#dfn-forward
// POST /session/{session id}/forward
Web::WebDriver::Response Client::forward(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/forward");
    auto session = TRY(Session::find_session(parameters[0]));

    auto response = TRY(session->traverse_history(1, Session::HandleUserPrompts::Yes));
    response = TRY(session->wait_for_navigation_completion());
    return response;
}

// 10.5 Refresh, https://w3c.github.io/webdriver/#dfn-refresh
// POST /session/{session id}/refresh
Web::WebDriver::Response Client::refresh(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/refresh");
    auto session = TRY(Session::find_session(parameters[0]));

    auto response = TRY(session->refresh());
    response = TRY(session->wait_for_navigation_completion());
    return response;
}

// 10.6 Get Title, https://w3c.github.io/webdriver/#dfn-get-title
// GET /session/{session id}/title
Web::WebDriver::Response Client::get_title(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/title");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_title"sv);
}

// Extension: POST /session/{session id}/ladybird/crash-current-page
Web::WebDriver::Response Client::crash_current_page(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/ladybird/crash-current-page");
    auto session = TRY(find_session_with_ladybird_test_hooks(parameters));

    auto wait_for_navigation_completion = true;
    if (payload.is_object())
        wait_for_navigation_completion = payload.as_object().get_bool("waitForNavigationCompletion"sv).value_or(true);

    TRY(session->run_content_command("crash_current_page"sv));
    if (!wait_for_navigation_completion)
        return JsonValue {};

    return session->wait_for_navigation_completion();
}

// Extension: POST /session/{session id}/ladybird/load-url-from-ui
Web::WebDriver::Response Client::load_url_from_ui(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/ladybird/load-url-from-ui");
    auto session = TRY(find_session_with_ladybird_test_hooks(parameters));

    if (!payload.is_object() || !payload.as_object().has_string("url"sv))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload doesn't have a string `url`"sv);
    auto url = URL::Parser::basic_parse(payload.as_object().get_string("url"sv).value());
    if (!url.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload has an invalid `url`"sv);

    auto response = TRY(session->load_url(url.release_value()));
    response = TRY(session->wait_for_navigation_completion());
    return response;
}

// Extension: POST /session/{session id}/ladybird/traverse-history-from-ui
Web::WebDriver::Response Client::traverse_history_from_ui(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/ladybird/traverse-history-from-ui");
    auto session = TRY(find_session_with_ladybird_test_hooks(parameters));

    auto wait_for_navigation_completion = true;
    if (payload.is_object())
        wait_for_navigation_completion = payload.as_object().get_bool("waitForNavigationCompletion"sv).value_or(true);

    if (!payload.is_object() || !payload.as_object().has_i32("delta"sv))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload doesn't have an integer `delta`"sv);

    auto response = TRY(session->traverse_history(payload.as_object().get_i32("delta"sv).value(), Session::HandleUserPrompts::No));

    if (!wait_for_navigation_completion)
        return JsonValue {};

    response = TRY(session->wait_for_navigation_completion());
    return response;
}

// Extension: GET /session/{session id}/ladybird/session-history
Web::WebDriver::Response Client::get_session_history(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/ladybird/session-history");
    auto session = TRY(find_session_with_ladybird_test_hooks(parameters));

    return session->session_history();
}

// 11.1 Get Window Handle, https://w3c.github.io/webdriver/#get-window-handle
// GET /session/{session id}/window
Web::WebDriver::Response Client::get_window_handle(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/window");
    auto session = TRY(Session::find_session(parameters[0]));

    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(session->run_content_command("ensure_top_level_browsing_context_is_open"sv));

    // 2. Return success with data being the window handle associated with the current top-level browsing context.
    return JsonValue { session->current_window_handle() };
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
// DELETE /session/{session id}/window
Web::WebDriver::Response Client::close_window(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/window");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->close_window();
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
// POST /session/{session id}/window
Web::WebDriver::Response Client::switch_to_window(Web::WebDriver::Parameters parameters, AK::JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window");
    auto session = TRY(Session::find_session(parameters[0], Web::WebDriver::SessionFlags::Default, Session::AllowInvalidWindowHandle::Yes));

    if (!payload.is_object())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload is not a JSON object"sv);

    // 1. Let handle be the result of getting the property "handle" from the parameters argument.
    auto handle = payload.as_object().get("handle"sv);

    // 2. If handle is undefined, return error with error code invalid argument.
    if (!handle.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "No property called 'handle' present"sv);

    return session->switch_to_window(handle->as_string());
}

// 11.4 Get Window Handles, https://w3c.github.io/webdriver/#dfn-get-window-handles
// GET /session/{session id}/window/handles
Web::WebDriver::Response Client::get_window_handles(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/window/handles");
    auto session = TRY(Session::find_session(parameters[0], Web::WebDriver::SessionFlags::Default, Session::AllowInvalidWindowHandle::Yes));
    return session->get_window_handles();
}

// 11.5 New Window, https://w3c.github.io/webdriver/#dfn-new-window
// POST /session/{session id}/window/new
Web::WebDriver::Response Client::new_window(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/new");
    auto session = TRY(Session::find_session(parameters[0]));

    auto handle = TRY(session->run_content_command("new_window"sv, move(payload)));

    static constexpr u32 CONNECTION_TIMEOUT_MS = 5000;
    auto timeout_fired = false;
    auto timer = Core::Timer::create_single_shot(CONNECTION_TIMEOUT_MS, [&timeout_fired] { timeout_fired = true; });
    timer->start();

    Core::EventLoop::current().spin_until([&session, &timeout_fired, handle = handle.as_object().get("handle"sv)->as_string()]() {
        return session->has_window_handle(handle) || timeout_fired;
    });

    if (timeout_fired)
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::Timeout, "Timed out waiting for window handle"sv);

    return handle;
}

// 11.6 Switch To Frame, https://w3c.github.io/webdriver/#dfn-switch-to-frame
// POST /session/{session id}/frame
Web::WebDriver::Response Client::switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/frame");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("switch_to_frame"sv, move(payload));
}

// 11.7 Switch To Parent Frame, https://w3c.github.io/webdriver/#dfn-switch-to-parent-frame
// POST /session/{session id}/frame/parent
Web::WebDriver::Response Client::switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/frame/parent");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("switch_to_parent_frame"sv, move(payload));
}

// 11.8.1 Get Window Rect, https://w3c.github.io/webdriver/#dfn-get-window-rect
// GET /session/{session id}/window/rect
Web::WebDriver::Response Client::get_window_rect(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/window/rect");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_window_rect"sv);
}

// 11.8.2 Set Window Rect, https://w3c.github.io/webdriver/#dfn-set-window-rect
// POST /session/{session id}/window/rect
Web::WebDriver::Response Client::set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/rect");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("set_window_rect"sv, move(payload));
}

// 11.8.3 Maximize Window, https://w3c.github.io/webdriver/#dfn-maximize-window
// POST /session/{session id}/window/maximize
Web::WebDriver::Response Client::maximize_window(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/maximize");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("maximize_window"sv);
}

// 11.8.4 Minimize Window, https://w3c.github.io/webdriver/#minimize-window
// POST /session/{session id}/window/minimize
Web::WebDriver::Response Client::minimize_window(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/minimize");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("minimize_window"sv);
}

// 11.8.5 Fullscreen Window, https://w3c.github.io/webdriver/#dfn-fullscreen-window
// POST /session/{session id}/window/fullscreen
Web::WebDriver::Response Client::fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/fullscreen");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("fullscreen_window"sv);
}

// Extension: Consume User Activation, https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
// POST /session/{session id}/window/consume-user-activation
Web::WebDriver::Response Client::consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/consume-user-activation");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->run_content_command("consume_user_activation"sv);
}

// 12.3.2 Find Element, https://w3c.github.io/webdriver/#dfn-find-element
// POST /session/{session id}/element
Web::WebDriver::Response Client::find_element(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("find_element"sv, move(payload));
}

// 12.3.3 Find Elements, https://w3c.github.io/webdriver/#dfn-find-elements
// POST /session/{session id}/elements
Web::WebDriver::Response Client::find_elements(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/elements");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("find_elements"sv, move(payload));
}

// 12.3.4 Find Element From Element, https://w3c.github.io/webdriver/#dfn-find-element-from-element
// POST /session/{session id}/element/{element id}/element
Web::WebDriver::Response Client::find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/element");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("find_element_from_element"sv, move(payload), { move(parameters[1]) });
}

// 12.3.5 Find Elements From Element, https://w3c.github.io/webdriver/#dfn-find-elements-from-element
// POST /session/{session id}/element/{element id}/elements
Web::WebDriver::Response Client::find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/elements");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("find_elements_from_element"sv, move(payload), { move(parameters[1]) });
}

// 12.3.6 Find Element From Shadow Root, https://w3c.github.io/webdriver/#find-element-from-shadow-root
// POST /session/{session id}/shadow/{shadow id}/element
Web::WebDriver::Response Client::find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/shadow/<shadow_id>/element");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("find_element_from_shadow_root"sv, move(payload), { move(parameters[1]) });
}

// 12.3.7 Find Elements From Shadow Root, https://w3c.github.io/webdriver/#find-elements-from-shadow-root
// POST /session/{session id}/shadow/{shadow id}/elements
Web::WebDriver::Response Client::find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/shadow/<shadow_id>/elements");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("find_elements_from_shadow_root"sv, move(payload), { move(parameters[1]) });
}

// 12.3.8 Get Active Element, https://w3c.github.io/webdriver/#get-active-element
// GET /session/{session id}/element/active
Web::WebDriver::Response Client::get_active_element(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/active");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_active_element"sv);
}

// 12.3.9 Get Element Shadow Root, https://w3c.github.io/webdriver/#get-element-shadow-root
// GET /session/{session id}/element/{element id}/shadow
Web::WebDriver::Response Client::get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/shadow");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_shadow_root"sv, {}, { move(parameters[1]) });
}

// 12.4.1 Is Element Selected, https://w3c.github.io/webdriver/#dfn-is-element-selected
// GET /session/{session id}/element/{element id}/selected
Web::WebDriver::Response Client::is_element_selected(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/selected");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("is_element_selected"sv, {}, { move(parameters[1]) });
}

// 12.4.2 Get Element Attribute, https://w3c.github.io/webdriver/#dfn-get-element-attribute
// GET /session/{session id}/element/{element id}/attribute/{name}
Web::WebDriver::Response Client::get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/attribute/<name>");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_attribute"sv, {}, { move(parameters[1]), move(parameters[2]) });
}

// 12.4.3 Get Element Property, https://w3c.github.io/webdriver/#dfn-get-element-property
// GET /session/{session id}/element/{element id}/property/{name}
Web::WebDriver::Response Client::get_element_property(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/property/<name>");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_property"sv, {}, { move(parameters[1]), move(parameters[2]) });
}

// 12.4.4 Get Element CSS Value, https://w3c.github.io/webdriver/#dfn-get-element-css-value
// GET /session/{session id}/element/{element id}/css/{property name}
Web::WebDriver::Response Client::get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/css/<property_name>");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_css_value"sv, {}, { move(parameters[1]), move(parameters[2]) });
}

// 12.4.5 Get Element Text, https://w3c.github.io/webdriver/#dfn-get-element-text
// GET /session/{session id}/element/{element id}/text
Web::WebDriver::Response Client::get_element_text(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/text");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_text"sv, {}, { move(parameters[1]) });
}

// 12.4.6 Get Element Tag Name, https://w3c.github.io/webdriver/#dfn-get-element-tag-name
// GET /session/{session id}/element/{element id}/name
Web::WebDriver::Response Client::get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/name");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_tag_name"sv, {}, { move(parameters[1]) });
}

// 12.4.7 Get Element Rect, https://w3c.github.io/webdriver/#dfn-get-element-rect
// GET /session/{session id}/element/{element id}/rect
Web::WebDriver::Response Client::get_element_rect(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/rect");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_element_rect"sv, {}, { move(parameters[1]) });
}

// 12.4.8 Is Element Enabled, https://w3c.github.io/webdriver/#dfn-is-element-enabled
// GET /session/{session id}/element/{element id}/enabled
Web::WebDriver::Response Client::is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/enabled");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("is_element_enabled"sv, {}, { move(parameters[1]) });
}

// 12.4.9 https://w3c.github.io/webdriver/#dfn-get-computed-role
// GET /session/{session id}/element/{element id}/computedrole
Web::WebDriver::Response Client::get_computed_role(Web::WebDriver::Parameters parameters, AK::JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session id>/element/<element id>/computedrole");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_computed_role"sv, {}, { move(parameters[1]) });
}

// 12.4.10 Get Computed Label, https://w3c.github.io/webdriver/#get-computed-label
// GET /session/{session id}/element/{element id}/computedlabel
Web::WebDriver::Response Client::get_computed_label(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session id>/element/<element id>/computedlabel");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_computed_label"sv, {}, { move(parameters[1]) });
}

// 12.5.1 Element Click, https://w3c.github.io/webdriver/#element-click
// POST /session/{session id}/element/{element id}/click
Web::WebDriver::Response Client::element_click(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/click");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("element_click"sv, {}, { move(parameters[1]) });
}

// 12.5.2 Element Clear, https://w3c.github.io/webdriver/#dfn-element-clear
// POST /session/{session id}/element/{element id}/clear
Web::WebDriver::Response Client::element_clear(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/clear");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("element_clear"sv, {}, { move(parameters[1]) });
}

// 12.5.3 Element Send Keys, https://w3c.github.io/webdriver/#dfn-element-send-keys
// POST /session/{session id}/element/{element id}/value
Web::WebDriver::Response Client::element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/value");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("element_send_keys"sv, move(payload), { move(parameters[1]) });
}

// 13.1 Get Page Source, https://w3c.github.io/webdriver/#dfn-get-page-source
// GET /session/{session id}/source
Web::WebDriver::Response Client::get_source(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/source");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_source"sv);
}

// 13.2.1 Execute Script, https://w3c.github.io/webdriver/#dfn-execute-script
// POST /session/{session id}/execute/sync
Web::WebDriver::Response Client::execute_script(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/execute/sync");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("execute_script"sv, move(payload));
}

// 13.2.2 Execute Async Script, https://w3c.github.io/webdriver/#dfn-execute-async-script
// POST /session/{session id}/execute/async
Web::WebDriver::Response Client::execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/execute/async");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("execute_async_script"sv, move(payload));
}

// 14.1 Get All Cookies, https://w3c.github.io/webdriver/#dfn-get-all-cookies
// GET /session/{session id}/cookie
Web::WebDriver::Response Client::get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/cookie");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_all_cookies"sv);
}

// 14.2 Get Named Cookie, https://w3c.github.io/webdriver/#dfn-get-named-cookie
// GET /session/{session id}/cookie/{name}
Web::WebDriver::Response Client::get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/cookie/<name>");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("get_named_cookie"sv, {}, { move(parameters[1]) });
}

// 14.3 Add Cookie, https://w3c.github.io/webdriver/#dfn-adding-a-cookie
// POST /session/{session id}/cookie
Web::WebDriver::Response Client::add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/cookie");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("add_cookie"sv, move(payload));
}

// 14.4 Delete Cookie, https://w3c.github.io/webdriver/#dfn-delete-cookie
// DELETE /session/{session id}/cookie/{name}
Web::WebDriver::Response Client::delete_cookie(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/cookie/<name>");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("delete_cookie"sv, {}, { move(parameters[1]) });
}

// 14.5 Delete All Cookies, https://w3c.github.io/webdriver/#dfn-delete-all-cookies
// DELETE /session/{session id}/cookie
Web::WebDriver::Response Client::delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/cookie");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("delete_all_cookies"sv);
}

// 15.7 Perform Actions, https://w3c.github.io/webdriver/#perform-actions
// POST /session/{session id}/actions
Web::WebDriver::Response Client::perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/actions");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("perform_actions"sv, move(payload));
}

// 15.8 Release Actions, https://w3c.github.io/webdriver/#release-actions
// DELETE /session/{session id}/actions
Web::WebDriver::Response Client::release_actions(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/actions");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("release_actions"sv);
}

// 16.1 Dismiss Alert, https://w3c.github.io/webdriver/#dismiss-alert
// POST /session/{session id}/alert/dismiss
Web::WebDriver::Response Client::dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/alert/dismiss");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("dismiss_alert"sv);
}

// 16.2 Accept Alert, https://w3c.github.io/webdriver/#accept-alert
// POST /session/{session id}/alert/accept
Web::WebDriver::Response Client::accept_alert(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/alert/accept");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("accept_alert"sv);
}

// 16.3 Get Alert Text, https://w3c.github.io/webdriver/#get-alert-text
// GET /session/{session id}/alert/text
Web::WebDriver::Response Client::get_alert_text(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/alert/text");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->run_content_command("get_alert_text"sv);
}

// 16.4 Send Alert Text, https://w3c.github.io/webdriver/#send-alert-text
// POST /session/{session id}/alert/text
Web::WebDriver::Response Client::send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/alert/text");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->run_content_command("send_alert_text"sv, move(payload));
}

// 17.1 Take Screenshot, https://w3c.github.io/webdriver/#take-screenshot
// GET /session/{session id}/screenshot
Web::WebDriver::Response Client::take_screenshot(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/screenshot");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("take_screenshot"sv);
}

// 17.2 Take Element Screenshot, https://w3c.github.io/webdriver/#dfn-take-element-screenshot
// GET /session/{session id}/element/{element id}/screenshot
Web::WebDriver::Response Client::take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/screenshot");
    auto session = TRY(Session::find_session(parameters[0]));

    return session->run_content_command("take_element_screenshot"sv, {}, { move(parameters[1]) });
}

// 18.1 Print Page, https://w3c.github.io/webdriver/#dfn-print-page
// POST /session/{session id}/print
Web::WebDriver::Response Client::print_page(Web::WebDriver::Parameters parameters, JsonValue payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session id>/print");
    auto session = TRY(Session::find_session(parameters[0]));
    return session->run_content_command("print_page"sv, move(payload));
}

}
