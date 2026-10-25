/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/JsonArray.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibURL/Parser.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebDriverBiDi.h>
#include <LibWebView/WebDriverBrowserConnection.h>

// Failures complete the command rather than propagate, so this macro may only be used in functions that take its
// completion as on_complete and return nothing.
#define TRY_OR_COMPLETE(expression)                                                                  \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (expression));                                                \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        if (_temporary_result.is_error()) [[unlikely]] {                                             \
            on_complete(_temporary_result.release_error());                                          \
            return;                                                                                  \
        }                                                                                            \
        _temporary_result.release_value();                                                           \
    })

namespace WebView {

using Web::WebDriver::as_js_uint;

WebDriverBiDi* WebDriverBiDi::s_the { nullptr };

// NB: The ids of the commands this object sends to processes hosting navigables are apart from those of the commands
//     WebDriver sends.
static constexpr u64 FIRST_CONTENT_COMMAND_ID = 1ull << 62;

// https://w3c.github.io/webdriver-bidi/#default-user-context
// FIXME: Support user contexts other than the default user context.
static constexpr auto DEFAULT_USER_CONTEXT_ID = "default"sv;

static Web::WebDriver::Error invalid_argument(StringView message)
{
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, message);
}

static Web::WebDriver::Error no_such_frame()
{
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchFrame, "Browsing context not found"sv);
}

static ErrorOr<String, Web::WebDriver::Error> get_string(JsonObject const& params, StringView name)
{
    auto value = params.get(name);
    if (!value.has_value() || !value->is_string())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a string", name)));
    return value->as_string();
}

static ErrorOr<Optional<String>, Web::WebDriver::Error> get_optional_string(JsonObject const& params, StringView name)
{
    if (!params.has(name))
        return Optional<String> {};
    return TRY(get_string(params, name));
}

static ErrorOr<Optional<bool>, Web::WebDriver::Error> get_optional_bool(JsonObject const& params, StringView name)
{
    auto value = params.get(name);
    if (!value.has_value())
        return Optional<bool> {};
    if (!value->is_bool())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a boolean", name)));
    return value->as_bool();
}

static ErrorOr<Optional<Vector<String>>, Web::WebDriver::Error> get_optional_non_empty_list_of_strings(JsonObject const& params, StringView name)
{
    auto value = params.get(name);
    if (!value.has_value())
        return Optional<Vector<String>> {};
    if (!value->is_array() || value->as_array().is_empty())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a non-empty list of strings", name)));

    Vector<String> result;
    for (auto const& item : value->as_array().values()) {
        if (!item.is_string())
            return invalid_argument(MUST(String::formatted("Parameter '{}' must be a non-empty list of strings", name)));
        result.append(item.as_string());
    }
    return result;
}

WebDriverBiDi::WebDriverBiDi(WebDriverBrowserConnection& connection)
    : m_connection(connection)
    , m_next_content_command_id(FIRST_CONTENT_COMMAND_ID)
{
    VERIFY(!s_the);
    s_the = this;
}

WebDriverBiDi::~WebDriverBiDi()
{
    s_the = nullptr;

    // NB: Web content runs no WebDriver BiDi steps once the session is gone.
    if (m_session_state.is_active) {
        WebContentClient::for_each_client([&](WebContentClient& client) {
            client.async_set_webdriver_bidi_session_state({});
            return IterationDecision::Continue;
        });
    }
}

void WebDriverBiDi::complete_command(u64 command_id, Web::WebDriver::Response response)
{
    m_connection.async_command_complete(command_id, move(response));
}

bool WebDriverBiDi::did_complete_content_command(u64 command_id, Web::WebDriver::Response& response)
{
    auto on_complete = m_pending_content_commands.take(command_id);
    if (!on_complete.has_value())
        return false;

    on_complete.value()(move(response));
    return true;
}

void WebDriverBiDi::ensure_session_is_active()
{
    if (!m_session_state.is_active)
        push_session_state();
}

void WebDriverBiDi::push_session_state()
{
    m_session_state.is_active = true;
    m_session_state.enabled_events.clear_with_capacity();
    for (auto const& event_name : m_enabled_events)
        m_session_state.enabled_events.append(event_name);

    WebContentClient::for_each_client([&](WebContentClient& client) {
        client.async_set_webdriver_bidi_session_state(m_session_state);
        return IterationDecision::Continue;
    });
}

void WebDriverBiDi::set_enabled_events(Vector<String> const& event_names)
{
    m_enabled_events.clear();
    for (auto const& event_name : event_names)
        m_enabled_events.set(event_name);

    push_session_state();
}

// https://w3c.github.io/webdriver-bidi/#type-browsingContext-Browsingcontext
// For navigables with an associated WebDriver window handle the navigable id must be the same as the window handle.
String WebDriverBiDi::navigable_id(CanonicalNavigable const& navigable)
{
    if (navigable.is_top_level_traversable()) {
        if (auto view = static_cast<CanonicalTraversable const&>(navigable).view(); view.has_value() && !view->handle().is_empty())
            return view->handle();
    }

    // NB: This is how WebContent names a frame to WebDriver.
    return MUST(String::formatted("{}-{}", navigable.id().namespace_id, navigable.id().local_id));
}

static bool is_top_level_traversable_of_a_window(ViewImplementation const& view)
{
    return !view.owner_view_id().has_value() && view.has_traversable() && !view.handle().is_empty();
}

static Vector<CanonicalTraversable&> all_top_level_traversables()
{
    // NB: The traversables are in the order they were created in, which is the order of their views' ids.
    Vector<ViewImplementation*> views;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (is_top_level_traversable_of_a_window(view))
            views.append(&view);
        return IterationDecision::Continue;
    });
    quick_sort(views, [](auto const* a, auto const* b) { return a->view_id() < b->view_id(); });

    Vector<CanonicalTraversable&> traversables;
    for (auto* view : views)
        traversables.append(view->traversable());
    return traversables;
}

// https://w3c.github.io/webdriver-bidi/#get-a-navigable
static ErrorOr<CanonicalNavigable*, Web::WebDriver::Error> get_a_navigable(StringView navigable_id)
{
    // 1. If navigable id is null, return success with data null.

    // 2. If there is no navigable with navigable id navigable id return error with error code no such frame
    // 3. Let navigable be the navigable with id navigable id.
    // 4. Return success with data navigable.
    if (auto view = ViewImplementation::find_view_by_handle(navigable_id); view.has_value() && is_top_level_traversable_of_a_window(*view))
        return &view->traversable();

    auto parts = navigable_id.split_view('-');
    if (parts.size() != 2)
        return no_such_frame();

    auto namespace_id = parts[0].to_number<u64>();
    auto local_id = parts[1].to_number<u64>();
    if (!namespace_id.has_value() || !local_id.has_value())
        return no_such_frame();

    Web::HTML::CrossProcessId id { .namespace_id = *namespace_id, .local_id = *local_id };
    auto* traversable = CanonicalTraversable::traversable_containing(id);
    if (!traversable)
        return no_such_frame();

    auto view = traversable->view();
    if (!view.has_value() || !is_top_level_traversable_of_a_window(*view))
        return no_such_frame();

    auto navigable = traversable->find(id);
    if (!navigable.has_value() || navigable->is_top_level_traversable())
        return no_such_frame();

    return &navigable.value();
}

static ErrorOr<CanonicalTraversable*, Web::WebDriver::Error> get_a_top_level_traversable(StringView navigable_id)
{
    auto* navigable = TRY(get_a_navigable(navigable_id));
    if (!navigable->is_top_level_traversable())
        return invalid_argument("Browsing context is not a top-level traversable"sv);
    return static_cast<CanonicalTraversable*>(navigable);
}

void WebDriverBiDi::emit_event(StringView name, JsonValue params, Vector<String> top_level_traversable_ids)
{
    m_connection.async_bidi_event(MUST(String::from_utf8(name)), move(params), move(top_level_traversable_ids), { MUST(String::from_utf8(DEFAULT_USER_CONTEXT_ID)) });
}

void WebDriverBiDi::emit_event_for_navigable(StringView name, JsonValue params, CanonicalNavigable const& navigable)
{
    emit_event(name, move(params), { navigable_id(navigable.top_level_traversable()) });
}

// https://w3c.github.io/webdriver-bidi/#get-the-navigable-info
JsonObject WebDriverBiDi::get_the_navigable_info(CanonicalNavigable const& navigable, Optional<u64> max_depth, bool include_parent_id) const
{
    // 1. Let navigable id be the navigable id for navigable.
    auto id = navigable_id(navigable);

    // 2. Let parent navigable be navigable's parent.
    auto const* parent_navigable = navigable.parent();

    // 3. If parent navigable is not null let parent id be the navigable id of parent navigable. Otherwise let parent id
    //    be null.
    JsonValue parent_id;
    if (parent_navigable)
        parent_id = navigable_id(*parent_navigable);

    // 4. Let document be navigable's active document.
    // 5. Let url be the result of running the URL serializer, given document's URL.
    auto url = navigable.active_document_url().serialize();

    // 6. Let child infos be null.
    JsonValue child_infos;

    // 7. If max depth is null, or max depth is greater than 0:
    if (!max_depth.has_value() || *max_depth > 0) {
        // 1. Let child navigables be get the child navigables given navigable.
        // 2. Let child depth be max depth - 1 if max depth is not null, or null otherwise.
        Optional<u64> child_depth;
        if (max_depth.has_value())
            child_depth = *max_depth - 1;

        // 3. Set child infos to an empty list.
        JsonArray infos;

        // 4. For each child navigable of child navigables:
        for (auto const& child_navigable : navigable.children()) {
            // 1. Let info be the result of get the navigable info given child navigable, child depth, and false.
            // 2. Append info to child infos
            infos.must_append(get_the_navigable_info(*child_navigable, child_depth, false));
        }
        child_infos = move(infos);
    }

    // 8. Let user context be navigable's associated user context.

    // 9. Let opener id be the navigable id for navigable's original opener, if navigable's original opener is not null,
    //    and null otherwise.
    JsonValue opener_id;
    if (auto opener = m_original_openers.get(navigable.id()); opener.has_value())
        opener_id = *opener;

    // 10. Let top-level traversable be navigable's top-level traversable.
    // 11. Let client window id be the client window id for top-level traversable's associated client window.
    // FIXME: Name the OS window a tab is in, rather than the tab.
    auto client_window_id = navigable_id(navigable.top_level_traversable());

    // 12. Let navigable info be a map matching the browsingContext.Info production with the context field set to
    //     navigable id, the parent field set to parent id if include parent id is true, or unset otherwise, the url
    //     field set to url, the userContext field set to user context's user context id, originalOpener field set to
    //     opener id, the children field set to child infos, and the clientWindow field set to client window id.
    JsonObject navigable_info;
    navigable_info.set("context"sv, move(id));
    if (include_parent_id)
        navigable_info.set("parent"sv, move(parent_id));
    navigable_info.set("url"sv, move(url));
    navigable_info.set("userContext"sv, DEFAULT_USER_CONTEXT_ID);
    navigable_info.set("originalOpener"sv, move(opener_id));
    navigable_info.set("children"sv, move(child_infos));
    navigable_info.set("clientWindow"sv, move(client_window_id));

    // 13. Return navigable info.
    return navigable_info;
}

// https://w3c.github.io/webdriver-bidi/#get-the-navigation-info
JsonObject WebDriverBiDi::get_the_navigation_info(CanonicalNavigable const& navigable, NavigationStatus const& navigation_status) const
{
    // 1. Let navigable id be the navigable id for navigable.
    // 2. Let navigation id be navigation status's id.
    // 3. Let timestamp be a time value representing the current date and time in UTC.
    // 4. Let url be navigation status's url.
    // 5. Let user context id be the user context id of navigable's associated user context.
    // 6. Return a map matching the browsingContext.NavigationInfo production, with the context field set to navigable
    //    id, the navigation field set to navigation id, the timestamp field set to timestamp, the url field set to the
    //    result of the URL serializer given url, and the userContext field set to user context id.
    JsonObject info;
    info.set("context"sv, navigable_id(navigable));
    if (navigation_status.id.has_value())
        info.set("navigation"sv, navigation_status.id->to_utf8());
    else
        info.set("navigation"sv, JsonValue {});
    info.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
    info.set("url"sv, navigation_status.url.serialize());
    info.set("userContext"sv, DEFAULT_USER_CONTEXT_ID);
    return info;
}

// https://w3c.github.io/webdriver-bidi/#await
void WebDriverBiDi::await(Vector<StringView> events, Web::HTML::CrossProcessId navigable_id, Utf16String const& resume_id, ResumeSteps algorithm)
{
    // 1. Pause the execution of algorithm.
    // 2. Assert: wait queue does not contain resume id.
    VERIFY(!m_wait_queue.contains(resume_id));

    // 3. Set wait queue[resume id] to (events, algorithm).
    m_wait_queue.set(resume_id, { move(events), navigable_id, move(algorithm) });
}

// https://w3c.github.io/webdriver-bidi/#resume
void WebDriverBiDi::resume(StringView name, Web::HTML::CrossProcessId navigable_id, Utf16String const& id, NavigationStatus const& parameters)
{
    // 1. If wait queue does not contain id, return.
    // NB: The events of another navigable, which another process may report, do not resume the steps.
    auto waiter = m_wait_queue.find(id);
    if (waiter == m_wait_queue.end() || waiter->value.navigable_id != navigable_id)
        return;

    // 2. Let (events, algorithm) be wait queue[id]
    // 3. For each event in events:
    for (auto event : waiter->value.events) {
        // 1. If event equals name:
        if (event == name) {
            // 1. Remove id from wait queue.
            auto algorithm = move(waiter->value.algorithm);
            m_wait_queue.remove(waiter);

            // 2. Resume running the steps in algorithm from the point at which they were paused, passing name and
            //    parameters as the result of the await.
            algorithm(name, parameters);
            return;
        }
    }
}

void WebDriverBiDi::fail_navigations_of(Function<bool(Web::HTML::CrossProcessId)> const& is_affected_navigable)
{
    Vector<Utf16String> navigation_ids;
    for (auto const& [navigation_id, waiter] : m_wait_queue) {
        if (is_affected_navigable(waiter.navigable_id))
            navigation_ids.append(navigation_id);
    }
    for (auto const& navigation_id : navigation_ids) {
        auto waiter = m_wait_queue.take(navigation_id);
        if (!waiter.has_value())
            continue;
        waiter->algorithm("navigation failed"sv, { .id = navigation_id, .status = NavigationStatusType::Canceled, .url = {} });
    }

    Vector<Web::HTML::CrossProcessId> reloading_navigable_ids;
    for (auto const& [navigable_id, waiter] : m_reload_waiters) {
        if (is_affected_navigable(navigable_id))
            reloading_navigable_ids.append(navigable_id);
    }
    for (auto const& navigable_id : reloading_navigable_ids) {
        if (auto waiter = m_reload_waiters.take(navigable_id); waiter.has_value())
            waiter->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation failed"sv));
    }
}

static bool navigable_exists(Web::HTML::CrossProcessId navigable_id)
{
    return CanonicalTraversable::traversable_containing(navigable_id) != nullptr;
}

// NB: This runs once the navigables that are going away are removed.
void WebDriverBiDi::remove_state_of_removed_navigables()
{
    fail_navigations_of([](auto navigable_id) { return !navigable_exists(navigable_id); });

    m_destroyed_navigables.remove_all_matching([](auto navigable_id) { return !navigable_exists(navigable_id); });
    m_closing_traversable_infos.remove_all_matching([](auto navigable_id, auto const&) { return !navigable_exists(navigable_id); });
}

void WebDriverBiDi::top_level_traversable_did_crash(CanonicalTraversable const& traversable)
{
    fail_navigations_of([&](auto navigable_id) { return CanonicalTraversable::traversable_containing(navigable_id) == &traversable; });
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigable-created
void WebDriverBiDi::navigable_created(CanonicalNavigable const& navigable, Optional<CanonicalNavigable const&> opener_navigable)
{
    // 1. Set navigable's original opener to opener navigable.
    if (opener_navigable.has_value())
        m_original_openers.set(navigable.id(), navigable_id(*opener_navigable));

    // NB: A top-level traversable is named by the window handle of its view, which it does not have yet.
    if (navigable.is_top_level_traversable())
        return;

    // 2. If the navigable cache behavior with navigable is "bypass", then perform implementation-defined steps to
    //    disable any implementation-specific resource caches for network requests originating from navigable.

    // 3. Let related navigables be a set containing navigable.
    // 4. For each session in the set of sessions for which an event is enabled given "browsingContext.contextCreated"
    //    and related navigables:
    if (!is_event_enabled("browsingContext.contextCreated"sv))
        return;

    // 1. Emit a context created event given session, navigable, and has planned navigation.
    // https://w3c.github.io/webdriver-bidi/#emit-a-context-created-event
    auto params = get_the_navigable_info(navigable, 0, true);
    params.set("hasPlannedNavigation"sv, false);
    emit_event_for_navigable("browsingContext.contextCreated"sv, move(params), navigable);
}

void WebDriverBiDi::top_level_traversable_did_get_window_handle(CanonicalTraversable const& traversable)
{
    if (!is_event_enabled("browsingContext.contextCreated"sv))
        return;

    // https://w3c.github.io/webdriver-bidi/#emit-a-context-created-event
    auto params = get_the_navigable_info(traversable, 0, true);
    params.set("hasPlannedNavigation"sv, false);
    emit_event_for_navigable("browsingContext.contextCreated"sv, move(params), traversable);
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigable-destroyed
void WebDriverBiDi::navigable_destroyed(CanonicalNavigable const& navigable)
{
    // NB: The navigable's info, which names its original opener, is reported before the navigable is forgotten.
    ScopeGuard forget_original_opener = [&] { m_original_openers.remove(navigable.id()); };

    Core::deferred_invoke([] {
        if (auto* webdriver_bidi = WebDriverBiDi::the())
            webdriver_bidi->remove_state_of_removed_navigables();
    });

    if (m_destroyed_navigables.remove(navigable.id()))
        return;

    if (!is_event_enabled("browsingContext.contextDestroyed"sv)) {
        m_closing_traversable_infos.remove(navigable.id());
        return;
    }

    // 1. Let params be the result of get the navigable info, given navigable, null, and true.
    auto params = m_closing_traversable_infos.take(navigable.id()).value_or_lazy_evaluated([&] {
        return get_the_navigable_info(navigable, {}, true);
    });

    // 2. Let body be a map matching the browsingContext.ContextDestroyed production, with the params field set to
    //    params.
    // 3. Let related navigables be a set containing navigable's parent, if that is not null, or an empty set otherwise.
    // INTEROP: The destruction of a top-level traversable is reported to the sessions subscribed to its events, which
    //          is what other implementations do, so the related navigables contain the traversable itself.
    Vector<String> top_level_traversable_ids;
    top_level_traversable_ids.append(navigable_id(navigable.top_level_traversable()));

    // 4. For each session in the set of sessions for which an event is enabled given
    //    "browsingContext.contextDestroyed" and related navigables:
    //     1. Emit an event with session and body.
    emit_event("browsingContext.contextDestroyed"sv, move(params), move(top_level_traversable_ids));
}

// NB: The navigables of a closing top-level traversable are removed before the traversable is destroyed, which reports
//     its info as it was.
void WebDriverBiDi::top_level_traversable_is_closing(CanonicalNavigable const& traversable)
{
    if (!is_event_enabled("browsingContext.contextDestroyed"sv) || m_closing_traversable_infos.contains(traversable.id()))
        return;
    m_closing_traversable_infos.set(traversable.id(), get_the_navigable_info(traversable, {}, true));
}

void WebDriverBiDi::top_level_traversable_did_close(String const& window_handle)
{
    auto os_window = os_window_of(window_handle);
    m_tab_windows.remove(window_handle);
    m_viewport_defaults.remove(window_handle);
    m_css_viewport_overrides.remove(window_handle);
    remove_state_of_removed_navigables();

    // NB: Closing the focused top-level traversable activates the last other one of its OS window, or the last other
    //     one there is, the way a browser activates another tab or window.
    auto traversables = all_top_level_traversables();
    if (!traversables.is_empty() && !any_of(traversables, [](auto const& traversable) { return traversable.has_system_focus(); })) {
        auto const* next_traversable = &traversables.last();
        for (auto const& traversable : traversables) {
            if (os_window_of(navigable_id(traversable)) == os_window)
                next_traversable = &traversable;
        }
        JsonObject params;
        params.set("context"sv, navigable_id(*next_traversable));
        browsing_context_activate(params, [](auto) { });
    }

    if (auto close_commands = m_pending_close_commands.take(window_handle); close_commands.has_value()) {
        for (auto& on_complete : *close_commands)
            on_complete(JsonValue {});
    }
}

void WebDriverBiDi::navigation_superseded_reload(CanonicalNavigable const& navigable)
{
    if (auto waiter = m_reload_waiters.take(navigable.id()); waiter.has_value())
        waiter->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation was canceled"sv));
}

void WebDriverBiDi::reload_did_not_apply(CanonicalTraversable const& traversable, Optional<Utf16String> const& navigation_id)
{
    auto it = m_reload_waiters.find(traversable.id());
    if (it == m_reload_waiters.end() || it->value.navigation_id != navigation_id)
        return;
    if (auto waiter = m_reload_waiters.take(traversable.id()); waiter.has_value())
        waiter->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation was canceled"sv));
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-started
void WebDriverBiDi::navigation_started(CanonicalNavigable const& navigable, NavigationStatus const& navigation_status)
{
    // 1. Let params be the result of get the navigation info given navigable and navigation status.
    // 2. Let body be a map matching the browsingContext.NavigationStarted production, with the params field set to
    //    params.
    auto params = get_the_navigation_info(navigable, navigation_status);

    // 3. Let navigation id be navigation status's id.
    // 4. Let related navigables be a set containing navigable.
    // 5. Resume with "navigation started", navigation id, and navigation status.
    // 6. For each session in the set of sessions for which an event is enabled given
    //    "browsingContext.navigationStarted" and related navigables:
    //     1. Emit an event with session and body.
    // NB: The event is emitted before resuming, as the steps resumed may complete a command, whose response the local
    //     end expects after the event.
    if (is_event_enabled("browsingContext.navigationStarted"sv))
        emit_event_for_navigable("browsingContext.navigationStarted"sv, move(params), navigable);
    if (navigation_status.id.has_value())
        resume("navigation started"sv, navigable.id(), *navigation_status.id, navigation_status);
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-failed
void WebDriverBiDi::navigation_failed(CanonicalNavigable const& navigable, NavigationStatus const& navigation_status)
{
    // 1. Let params be the result of get the navigation info given navigable and navigation status.
    // 2. Let body be a map matching the browsingContext.NavigationFailed production, with the params field set to
    //    params.
    auto params = get_the_navigation_info(navigable, navigation_status);

    // 3. Let navigation id be navigation status's id.
    // 4. Let related navigables be a set containing navigable.
    // 5. Resume with "navigation failed", navigation id, and navigation status.
    // 6. For each session in the set of sessions for which an event is enabled given
    //    "browsingContext.navigationFailed" and related navigables:
    //     1. Emit an event with session and body.
    // NB: The event is emitted before resuming, as the steps resumed may complete a command, whose response the local
    //     end expects after the event.
    if (is_event_enabled("browsingContext.navigationFailed"sv))
        emit_event_for_navigable("browsingContext.navigationFailed"sv, move(params), navigable);
    if (navigation_status.id.has_value())
        resume("navigation failed"sv, navigable.id(), *navigation_status.id, navigation_status);
}

void WebDriverBiDi::did_receive_navigation_event(WebContentPage& page, Web::HTML::CrossProcessId navigable_id, String const& name, Optional<Utf16String> navigation_id, URL::URL url)
{
    // NB: A process hosting web content only reports on the navigables it hosts.
    auto navigable = page.hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;

    // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigable-destroyed
    // NB: The process hosting the container of a child navigable reports its destruction before the navigable and its
    //     descendants are removed, which reports them no more.
    if (name == "browsingContext.contextDestroyed"sv) {
        if (navigable->is_top_level_traversable())
            return;
        navigable_destroyed(*navigable);
        Function<void(CanonicalNavigable const&)> mark_as_destroyed = [&](CanonicalNavigable const& destroyed_navigable) {
            m_destroyed_navigables.set(destroyed_navigable.id());
            for (auto const& child : destroyed_navigable.children())
                mark_as_destroyed(*child);
        };
        mark_as_destroyed(*navigable);
        return;
    }

    // https://w3c.github.io/webdriver-bidi/#event-browsingContext-historyUpdated
    if (name == "browsingContext.historyUpdated"sv) {
        if (!is_event_enabled(name))
            return;

        // 1. Let url be the result of running the URL serializer, given navigable's active browsing context's active
        //    document's URL.
        // 2. Let user context id be the user context id of navigable's associated user context.
        // 3. Let timestamp be a time value representing the current date and time in UTC.
        // 4. Let params be a map matching the browsingContext.HistoryUpdatedParameters production, with the url field
        //    set to url, the timestamp field set to timestamp, the context field set to navigable's navigable id and
        //    the userContext field set to user context id.
        JsonObject params;
        params.set("context"sv, WebDriverBiDi::navigable_id(*navigable));
        params.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
        params.set("url"sv, url.serialize());
        params.set("userContext"sv, DEFAULT_USER_CONTEXT_ID);

        // 5. Let body be a map matching the browsingContext.HistoryUpdated production, with the params field set to
        //    params.
        // 6. Let related browsing contexts be a set containing navigable's active browsing context.
        // 7. For each session in the set of sessions for which an event is enabled given
        //    "browsingContext.historyUpdated" and related browsing contexts:
        //     1. Emit an event with session and body.
        emit_event_for_navigable(name, move(params), *navigable);
        return;
    }

    // NB: These are the events whose triggers run in the process hosting a navigable's active document.
    struct NavigationEvent {
        StringView event_name;
        StringView resume_name;
        NavigationStatusType status;
    };
    static constexpr auto navigation_events = to_array<NavigationEvent>({
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-started
        { "browsingContext.navigationStarted"sv, "navigation started"sv, NavigationStatusType::Pending },
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-fragment-navigated
        { "browsingContext.fragmentNavigated"sv, "fragment navigated"sv, NavigationStatusType::Complete },
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-committed
        { "browsingContext.navigationCommitted"sv, "navigation committed"sv, NavigationStatusType::Pending },
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-dom-content-loaded
        { "browsingContext.domContentLoaded"sv, "domContentLoaded"sv, NavigationStatusType::Pending },
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-load-complete
        { "browsingContext.load"sv, "load"sv, NavigationStatusType::Complete },
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-aborted
        { "browsingContext.navigationAborted"sv, "navigation aborted"sv, NavigationStatusType::Canceled },
        // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-failed
        { "browsingContext.navigationFailed"sv, "navigation failed"sv, NavigationStatusType::Canceled },
    });

    NavigationEvent const* event = nullptr;
    for (auto const& navigation_event : navigation_events) {
        if (navigation_event.event_name == name)
            event = &navigation_event;
    }
    if (!event)
        return;

    NavigationStatus navigation_status { .id = move(navigation_id), .status = event->status, .url = move(url) };

    // 1. Let params be the result of get the navigation info given navigable and navigation status.
    auto params = get_the_navigation_info(*navigable, navigation_status);

    // 2. Let navigation id be navigation status's id.
    // 3. Let related navigables be a set containing navigable.
    // 4. Resume with the event's name, navigation id, and navigation status.
    // 5. For each session in the set of sessions for which an event is enabled given the event's name and related
    //    navigables:
    //     1. Emit an event with session and body.
    // NB: The event is emitted before resuming, as the steps resumed may complete a command, whose response the local
    //     end expects after the event.
    if (is_event_enabled(event->event_name))
        emit_event_for_navigable(event->event_name, move(params), *navigable);
    if (navigation_status.id.has_value())
        resume(event->resume_name, navigable->id(), *navigation_status.id, navigation_status);

    // NB: Events from an older document or a different navigation cannot complete this reload.
    if (auto waiter = m_reload_waiters.find(navigable->id()); waiter != m_reload_waiters.end() && navigation_status.id == waiter->value.navigation_id) {
        auto failed = name.is_one_of("browsingContext.navigationAborted"sv, "browsingContext.navigationFailed"sv);
        auto wait_condition = waiter->value.wait_condition;
        auto reached = (wait_condition == WaitCondition::Committed && name == "browsingContext.navigationCommitted"sv)
            || (wait_condition == WaitCondition::Interactive && name == "browsingContext.domContentLoaded"sv)
            || (wait_condition == WaitCondition::Complete && name == "browsingContext.load"sv);

        if (failed || reached) {
            auto on_complete = move(waiter->value.on_complete);
            m_reload_waiters.remove(waiter);

            if (failed) {
                on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation failed"sv));
                return;
            }

            JsonObject body;
            if (navigation_status.id.has_value())
                body.set("navigation"sv, navigation_status.id->to_utf8());
            else
                body.set("navigation"sv, JsonValue {});
            body.set("url"sv, navigation_status.url.serialize());
            on_complete(JsonValue { move(body) });
        }
    }
}

void WebDriverBiDi::did_receive_event(WebContentPage& page, Web::HTML::CrossProcessId navigable_id, String const& name, JsonValue params)
{
    // NB: A process hosting web content only reports on the navigables it hosts, and only the events that run there.
    //     The realms of a closing traversable's navigables are destroyed after the navigables are removed, and they
    //     are reported for the traversable instead.
    auto navigable = page.hosted_navigable(navigable_id);
    if (!navigable.has_value() && name == "script.realmDestroyed"sv)
        navigable = page.traversable();
    if (!navigable.has_value())
        return;

    static constexpr auto content_events = to_array<StringView>({
        "log.entryAdded"sv,
        "script.message"sv,
        "script.realmCreated"sv,
        "script.realmDestroyed"sv,
    });
    if (!any_of(content_events, [&](auto event_name) { return event_name == name; }))
        return;

    // NB: WebDriver buffers the log events that no subscription enables.
    if (!is_event_enabled(name) && name != "log.entryAdded"sv)
        return;

    // NB: The navigable an event names is the navigable the process reported it for, which the browser process names
    //     itself, so that a process cannot attribute an event to a navigable it does not host.
    if (!params.is_object())
        return;
    auto set_context = [&](JsonObject& object) {
        if (object.has("context"sv))
            object.set("context"sv, WebDriverBiDi::navigable_id(*navigable));
    };
    if (name.is_one_of("log.entryAdded"sv, "script.message"sv)) {
        if (auto source = params.as_object().get_object("source"sv); source.has_value())
            set_context(*source);
    } else if (name == "script.realmCreated"sv) {
        set_context(params.as_object());
    }

    emit_event_for_navigable(name, move(params), *navigable);
}

void WebDriverBiDi::get_top_level_traversables(u64 command_id, Vector<String> const& navigable_ids)
{
    // https://w3c.github.io/webdriver-bidi/#get-valid-navigables-by-ids
    // https://w3c.github.io/webdriver-bidi/#get-top-level-traversables
    HashTable<String> seen;
    JsonArray result;

    for (auto const& id : navigable_ids) {
        auto navigable = get_a_navigable(id);
        if (navigable.is_error()) {
            complete_command(command_id, navigable.release_error());
            return;
        }

        auto top_level_traversable_id = navigable_id(navigable.value()->top_level_traversable());
        if (seen.set(top_level_traversable_id) == HashSetResult::InsertedNewEntry)
            result.must_append(move(top_level_traversable_id));
    }

    complete_command(command_id, JsonValue { move(result) });
}

void WebDriverBiDi::run_subscribe_steps(u64 command_id, String const& event_name, Vector<String> const& top_level_traversable_ids)
{
    // https://w3c.github.io/webdriver-bidi/#event-browsingContext-contextCreated
    if (event_name == "browsingContext.contextCreated"sv) {
        JsonArray events;

        // https://w3c.github.io/webdriver-bidi/#recursively-emit-context-created-events
        Function<void(CanonicalNavigable const&)> recursively_emit_context_created_events = [&](CanonicalNavigable const& navigable) {
            // 1. Emit a context created event with session, navigable, and false.
            auto params = get_the_navigable_info(navigable, 0, true);
            params.set("hasPlannedNavigation"sv, false);

            JsonObject event;
            event.set("method"sv, "browsingContext.contextCreated"sv);
            event.set("params"sv, move(params));
            events.must_append(move(event));

            // 2. For each child navigable, child, of navigable:
            for (auto const& child : navigable.children()) {
                // 1. Recursively emit context created events given session and child.
                recursively_emit_context_created_events(*child);
            }
        };

        // 1. For each navigable in navigables:
        //     1. Recursively emit context created events given session and navigable.
        // NB: In the order of the top-level traversables.
        for (auto& traversable : all_top_level_traversables()) {
            if (top_level_traversable_ids.contains_slow(navigable_id(traversable)))
                recursively_emit_context_created_events(traversable);
        }

        complete_command(command_id, JsonValue { move(events) });
        return;
    }

    // https://w3c.github.io/webdriver-bidi/#event-script-realmCreated
    if (event_name == "script.realmCreated"sv) {
        // NB: The processes hosting the navigables' documents hold their realms. WebDriver gives the top-level
        //     traversables the subscription newly enables the event for, whether or not it is global.

        Vector<CanonicalNavigable const*> navigables;
        for (auto& traversable : all_top_level_traversables()) {
            if (!top_level_traversable_ids.contains_slow(navigable_id(traversable)))
                continue;
            traversable.for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
                navigables.append(&navigable);
                return IterationDecision::Continue;
            });
        }

        struct State : RefCounted<State> {
            Vector<Optional<JsonArray>> realms;
            size_t remaining { 0 };
            u64 command_id { 0 };
        };
        auto state = adopt_ref(*new State);
        state->realms.resize(navigables.size());
        state->remaining = navigables.size();
        state->command_id = command_id;

        auto finish = [this, state] {
            JsonArray events;
            for (auto& realms : state->realms) {
                if (!realms.has_value())
                    continue;
                for (auto& realm : realms->values()) {
                    JsonObject event;
                    event.set("method"sv, "script.realmCreated"sv);
                    event.set("params"sv, realm);
                    events.must_append(move(event));
                }
            }
            complete_command(state->command_id, JsonValue { move(events) });
        };

        if (navigables.is_empty()) {
            finish();
            return;
        }

        for (size_t i = 0; i < navigables.size(); ++i) {
            JsonObject params;
            params.set("context"sv, navigable_id(*navigables[i]));
            run_content_command(*navigables[i], "script.getRealms"_string, move(params), [state, i, finish](Web::WebDriver::Response response) {
                if (!response.is_error() && response.value().is_object()) {
                    if (auto realms = response.value().as_object().get_array("realms"sv); realms.has_value())
                        state->realms[i] = *realms;
                }
                if (--state->remaining == 0)
                    finish();
            });
        }
        return;
    }

    complete_command(command_id, JsonValue { JsonArray {} });
}

void WebDriverBiDi::run_content_command(CanonicalNavigable const& navigable, String const& method, JsonValue params, CommandResult on_complete)
{
    auto view = navigable.top_level_traversable().view();
    if (!view.has_value()) {
        on_complete(no_such_frame());
        return;
    }

    auto command_id = m_next_content_command_id++;
    m_pending_content_commands.set(command_id, move(on_complete));
    view->run_webdriver_bidi_content_command(command_id, navigable.id(), method, move(params));
}

void WebDriverBiDi::run_command(u64 command_id, String const& method, JsonValue params_value)
{
    ensure_session_is_active();

    // NB: A command may complete after the connection to WebDriver, and this object with it, went away.
    auto on_complete = [command_id](Web::WebDriver::Response response) {
        if (auto* webdriver_bidi = WebDriverBiDi::the())
            webdriver_bidi->complete_command(command_id, move(response));
    };

    if (!params_value.is_object()) {
        on_complete(invalid_argument("Command parameters must be an object"sv));
        return;
    }
    auto const& params = params_value.as_object();

    if (method == "browsingContext.activate"sv)
        return browsing_context_activate(params, move(on_complete));
    if (method == "browsingContext.close"sv)
        return browsing_context_close(params, move(on_complete));
    if (method == "browsingContext.create"sv)
        return browsing_context_create(params, move(on_complete));
    if (method == "browsingContext.getTree"sv)
        return browsing_context_get_tree(params, move(on_complete));
    if (method == "browsingContext.navigate"sv)
        return browsing_context_navigate(params, move(on_complete));
    if (method == "browsingContext.reload"sv)
        return browsing_context_reload(params, move(on_complete));
    if (method == "browsingContext.setViewport"sv)
        return browsing_context_set_viewport(params, move(on_complete));
    if (method == "browsingContext.traverseHistory"sv)
        return browsing_context_traverse_history(params, move(on_complete));

    if (method == "script.addPreloadScript"sv)
        return script_add_preload_script(params, move(on_complete));
    if (method == "script.removePreloadScript"sv)
        return script_remove_preload_script(params, move(on_complete));
    if (method == "script.getRealms"sv)
        return script_get_realms(params, move(on_complete));
    if (method.is_one_of("script.callFunction"sv, "script.disown"sv, "script.evaluate"sv))
        return route_to_script_target(method, params, move(on_complete));

    on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownCommand, "Command is not supported"sv));
}

// NB: Every top-level traversable that is not a tab created in the OS window of another one has an OS window of its
//     own, which is named by the traversable's id.
String WebDriverBiDi::os_window_of(String const& top_level_traversable_id) const
{
    return m_tab_windows.get(top_level_traversable_id).value_or(top_level_traversable_id);
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-activate
void WebDriverBiDi::browsing_context_activate(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let navigable id be the value of the command parameters["context"] field.
    auto navigable_id = TRY_OR_COMPLETE(get_string(params, "context"sv));

    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    // 3. If navigable is not a top-level traversable, return error with error code invalid argument.
    auto* traversable = TRY_OR_COMPLETE(get_a_top_level_traversable(navigable_id));

    // 4. Return activate a navigable with navigable.
    // https://w3c.github.io/webdriver-bidi/#activate-a-navigable
    auto view = traversable->view();

    // 1. Run implementation-specific steps so that navigable's system visibility state becomes visible. If this is not
    //    possible return error with error code unsupported operation.
    // NB: The other tabs of its OS window become hidden.
    auto os_window = os_window_of(navigable_id);
    for (auto& other_traversable : all_top_level_traversables()) {
        auto other_id = WebDriverBiDi::navigable_id(other_traversable);
        if (&other_traversable == traversable || os_window_of(other_id) != os_window)
            continue;
        if (auto other_view = other_traversable.view(); other_view.has_value())
            other_view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
    }
    view->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    if (view->on_activate_tab)
        view->on_activate_tab();

    // 2. Run implementation-specific steps to set the system focus on the navigable if it is not focused.
    // NB: Other top-level traversables lose the system focus.
    for (auto& other_traversable : all_top_level_traversables()) {
        if (&other_traversable != traversable)
            other_traversable.set_has_system_focus(false, nullptr);
    }
    traversable->set_has_system_focus(true, nullptr);

    // 3. Return success with data null.
    on_complete(JsonValue {});
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-close
void WebDriverBiDi::browsing_context_close(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let navigable id be the value of the context field of command parameters.
    auto navigable_id = TRY_OR_COMPLETE(get_string(params, "context"sv));

    // 2. Let prompt unload be the value of the promptUnload field of command parameters.
    auto prompt_unload = TRY_OR_COMPLETE(get_optional_bool(params, "promptUnload"sv)).value_or(false);

    // 3. Let navigable be the result of trying to get a navigable with navigable id.
    // 4. Assert: navigable is not null.
    // 5. If navigable is not a top-level traversable, return error with error code invalid argument.
    auto* traversable = TRY_OR_COMPLETE(get_a_top_level_traversable(navigable_id));
    auto view = traversable->view();

    // 6. If prompt unload is true:
    if (prompt_unload) {
        // 1. Close navigable.
        view->request_close();

        // NB: A prompt to unload can keep the navigable open, so the command does not wait for it to close.
        on_complete(JsonValue {});
        return;
    }

    // 7. Otherwise:
    //     1. Close navigable without prompting to unload.
    // 8. Return success with data null.
    // NB: The command completes once the navigable is closed.
    m_pending_close_commands.ensure(view->handle()).append(move(on_complete));
    view->prepare_for_close_without_prompting()();
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-create
void WebDriverBiDi::browsing_context_create(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let type be the value of the type field of command parameters.
    auto type = TRY_OR_COMPLETE(get_string(params, "type"sv));
    if (!type.is_one_of("tab"sv, "window"sv))
        return on_complete(invalid_argument("Parameter 'type' must be \"tab\" or \"window\""sv));

    // 2. Let reference navigable id be the value of the referenceContext field of command parameters, if present, or
    //    null otherwise.
    auto reference_navigable_id = TRY_OR_COMPLETE(get_optional_string(params, "referenceContext"sv));

    auto background = TRY_OR_COMPLETE(get_optional_bool(params, "background"sv)).value_or(false);

    // 3. If reference navigable id is not null, let reference navigable be the result of trying to get a navigable with
    //    reference navigable id. Otherwise let reference navigable be null.
    // 4. If reference navigable is not null and is not a top-level traversable, return error with error code invalid
    //    argument.
    CanonicalTraversable* reference_navigable = nullptr;
    if (reference_navigable_id.has_value())
        reference_navigable = TRY_OR_COMPLETE(get_a_top_level_traversable(*reference_navigable_id));

    // 5. If the implementation is unable to create a new top-level traversable for any reason then return error with
    //    error code unsupported operation.

    // 6. Let user context be the default user context if reference navigable is null, and reference navigable'
    //    associated user context otherwise.
    // 7. Let user context id be the value of the userContext field of command parameters if present, or null otherwise.
    auto user_context_id = TRY_OR_COMPLETE(get_optional_string(params, "userContext"sv));

    // 8. If user context id is not null, set user context to the result of trying to get user context with user
    //    context id.
    // 9. If user context is null, return error with error code no such user context.
    if (user_context_id.has_value() && *user_context_id != DEFAULT_USER_CONTEXT_ID)
        return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchUserContext, "User context not found"sv));

    // 10. If the implementation is unable to create a new top-level traversable with associated user context user
    //     context for any reason, return error with error code unsupported operation.

    // 11. Let traversable be the result of trying to create a new top-level traversable steps with null, empty string,
    //     null, and false, and setting the associated user context for the newly created top-level traversable to user
    //     context.
    // NB: The process hosting the reference navigable's document, or any top-level traversable's, opens the new
    //     top-level traversable the way the browser opens a window for a page.
    if (!reference_navigable) {
        auto traversables = all_top_level_traversables();
        if (traversables.is_empty())
            return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "No browsing context to create a new one from"sv));
        reference_navigable = &traversables.first();
    }

    JsonObject create_params;
    create_params.set("type"sv, type);
    auto reference_handle = navigable_id(*reference_navigable);

    // NB: The process names the top-level traversable it created, which is not one of those that exist now.
    HashTable<String> existing_handles;
    for (auto& traversable : all_top_level_traversables())
        existing_handles.set(navigable_id(traversable));

    run_content_command(*reference_navigable, "browsingContext.create"_string, move(create_params), [this, background, is_tab = type == "tab"sv, reference_handle = move(reference_handle), existing_handles = move(existing_handles), on_complete = move(on_complete)](Web::WebDriver::Response response) mutable {
        if (response.is_error())
            return on_complete(move(response));

        Optional<String> handle;
        if (response.value().is_object()) {
            if (auto context = response.value().as_object().get_string("context"sv); context.has_value())
                handle = *context;
        }
        if (!handle.has_value() || existing_handles.contains(*handle) || get_a_top_level_traversable(*handle).is_error())
            return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Unable to create a top-level traversable"sv));

        // NB: A tab shares the OS window of the reference navigable, which only shows its active tab.
        if (is_tab)
            m_tab_windows.set(*handle, os_window_of(reference_handle));

        // NB: Opening the traversable activated it.
        if (background) {
            if (auto new_view = get_a_top_level_traversable(*handle).value()->view(); new_view.has_value()) {
                new_view->traversable().set_has_system_focus(false, nullptr);
                if (is_tab)
                    new_view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
            }
            JsonObject reference_params;
            reference_params.set("context"sv, reference_handle);
            browsing_context_activate(reference_params, [](auto) { });
        }

        // 12. If the value of the command parameters' background field is false:
        if (!background) {
            // 1. Let activate result be the result of activate a navigable with the newly created navigable.
            // 2. If activate result is an error, return activate result.
            JsonObject activate_params;
            activate_params.set("context"sv, *handle);
            browsing_context_activate(activate_params, [](auto) { });
        }

        // 13. Let body be a map matching the browsingContext.CreateResult production, with the context field set to
        //     traversable's navigable id and the userContext property set to the user context id of traversable's
        //     associated user context.
        JsonObject body;
        body.set("context"sv, *handle);
        body.set("userContext"sv, DEFAULT_USER_CONTEXT_ID);

        // 14. Return success with data body.
        on_complete(JsonValue { move(body) });
    });
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-getTree
void WebDriverBiDi::browsing_context_get_tree(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let root id be the value of the root field of command parameters if present, or null otherwise.
    auto root_id = TRY_OR_COMPLETE(get_optional_string(params, "root"sv));

    // 2. Let max depth be the value of the maxDepth field of command parameters if present, or null otherwise.
    Optional<u64> max_depth;
    if (auto value = params.get("maxDepth"sv); value.has_value()) {
        max_depth = as_js_uint(*value);
        if (!max_depth.has_value())
            return on_complete(invalid_argument("Parameter 'maxDepth' must be an unsigned integer"sv));
    }

    // 3. Let navigables be an empty list.
    Vector<CanonicalNavigable const*> navigables;

    // 4. If root id is not null, append the result of trying to get a navigable given root id to navigables. Otherwise
    //    append all top-level traversables to navigables.
    if (root_id.has_value()) {
        navigables.append(TRY_OR_COMPLETE(get_a_navigable(*root_id)));
    } else {
        for (auto& traversable : all_top_level_traversables())
            navigables.append(&traversable);
    }

    // 5. Let navigables infos be an empty list.
    JsonArray navigables_infos;

    // 6. For each navigable of navigables:
    for (auto const* navigable : navigables) {
        // 1. Let info be the result of get the navigable info given navigable, max depth, and true.
        // 2. Append info to navigables infos
        navigables_infos.must_append(get_the_navigable_info(*navigable, max_depth, true));
    }

    // 7. Let body be a map matching the browsingContext.GetTreeResult production, with the contexts field set to
    //    navigables infos.
    JsonObject body;
    body.set("contexts"sv, move(navigables_infos));

    // 8. Return success with data body.
    on_complete(JsonValue { move(body) });
}

static ErrorOr<Optional<StringView>, Web::WebDriver::Error> get_wait_condition(JsonObject const& params)
{
    auto wait = TRY(get_optional_string(params, "wait"sv));
    if (!wait.has_value())
        return Optional<StringView> {};

    // browsingContext.ReadinessState = "none" / "interactive" / "complete"
    for (auto readiness_state : { "none"sv, "interactive"sv, "complete"sv }) {
        if (*wait == readiness_state)
            return readiness_state;
    }
    return invalid_argument("Parameter 'wait' must be a readiness state"sv);
}

// https://w3c.github.io/webdriver-bidi/#await-a-navigation
void WebDriverBiDi::await_a_navigation(CanonicalNavigable& navigable, URL::URL url, WaitCondition wait_condition, CommandResult on_complete)
{
    // 1. Let navigation id be the string representation of a UUID based on truly random, or pseudo-random numbers.
    // 2. Navigate navigable with resource request, and using navigable's active document as the source Document, with
    //    navigation id navigation id, and history handling behavior history handling. If ignore cache is true, the
    //    navigation must not load resources from the HTTP cache.
    // NB: The browser's UI navigates the navigable, which generates the navigation id.
    auto navigation = navigable.prepare_navigation(move(url));
    auto navigation_id = navigation.navigation_id;
    auto navigable_id = navigable.id();

    // 3. Let (event received, navigation status) be await given «"navigation started", "navigation failed",
    //    "fragment navigated"», and navigation id.
    await({ "navigation started"sv, "navigation failed"sv, "fragment navigated"sv }, navigable_id, navigation_id, [this, navigable_id, navigation_id, wait_condition, on_complete = move(on_complete)](StringView, NavigationStatus navigation_status) mutable {
        // 4. Assert: navigation status's id is navigation id.
        VERIFY(navigation_status.id == navigation_id);

        // 5. If navigation status's status is "complete":
        if (navigation_status.status == NavigationStatusType::Complete) {
            // 1. Let body be a map matching the browsingContext.NavigateResult production, with the navigation field set
            //    to navigation id, and the url field set to the result of the URL serializer given navigation status's
            //    url.
            JsonObject body;
            body.set("navigation"sv, navigation_id.to_utf8());
            body.set("url"sv, navigation_status.url.serialize());

            // 2. Return success with data body.
            on_complete(JsonValue { move(body) });
            return;
        }

        // 6. If navigation status's status is "canceled" return error with error code unknown error.
        if (navigation_status.status == NavigationStatusType::Canceled) {
            on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation was canceled"sv));
            return;
        }

        // 7. Assert: navigation status's status is "pending" and navigation id is not null.
        VERIFY(navigation_status.status == NavigationStatusType::Pending);

        await_navigation_completion(navigable_id, navigation_id, wait_condition, move(on_complete));
    });

    if (navigable.is_top_level_traversable()) {
        if (auto view = static_cast<CanonicalTraversable&>(navigable).view(); view.has_value()) {
            view->load(move(navigation));
            return;
        }
    }
    navigable.begin_navigation(move(navigation));
}

void WebDriverBiDi::await_navigation_completion(Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_id, WaitCondition wait_condition, CommandResult on_complete)
{
    // 8. If wait condition is "committed", let event name be "committed".
    // 9. Otherwise, if wait condition is "interactive", let event name be "domContentLoaded".
    // 10. Otherwise, let event name be "load".
    auto event_name = "load"sv;
    if (wait_condition == WaitCondition::Committed)
        event_name = "navigation committed"sv;
    else if (wait_condition == WaitCondition::Interactive)
        event_name = "domContentLoaded"sv;

    // 11. Let (event received, status) be await given «event name, "download started", "navigation aborted",
    //     "navigation failed"» and navigation id.
    await({ event_name, "download started"sv, "navigation aborted"sv, "navigation failed"sv }, navigable_id, navigation_id, [on_complete = move(on_complete)](StringView event_received, NavigationStatus status) {
        // 12. If event received is "navigation failed" return error with error code unknown error.
        if (event_received == "navigation failed"sv) {
            on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation failed"sv));
            return;
        }

        // 13. Let body be a map matching the browsingContext.NavigateResult production, with the navigation field set to
        //     status's id, and the url field set to the result of the URL serializer given status's url.
        JsonObject body;
        if (status.id.has_value())
            body.set("navigation"sv, status.id->to_utf8());
        else
            body.set("navigation"sv, JsonValue {});
        body.set("url"sv, status.url.serialize());

        // 14. Return success with data body.
        on_complete(JsonValue { move(body) });
    });
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-navigate
void WebDriverBiDi::browsing_context_navigate(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let navigable id be the value of the context field of command parameters.
    auto navigable_id = TRY_OR_COMPLETE(get_string(params, "context"sv));
    auto url = TRY_OR_COMPLETE(get_string(params, "url"sv));
    auto wait = TRY_OR_COMPLETE(get_wait_condition(params));

    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    // 3. Assert: navigable is not null.
    auto* navigable = TRY_OR_COMPLETE(get_a_navigable(navigable_id));

    // 4. Let wait condition be "committed".
    auto wait_condition = WaitCondition::Committed;

    // 5. If command parameters contains wait and command parameters[wait] is not "none", set wait condition to command
    //    parameters[wait].
    if (wait == "interactive"sv)
        wait_condition = WaitCondition::Interactive;
    else if (wait == "complete"sv)
        wait_condition = WaitCondition::Complete;

    // 6. Let url be the value of the url field of command parameters.
    // 7. Let document be navigable's active document.
    // 8. Let base be document's base URL.
    // FIXME: The browser process does not know a document's base URL, which a <base> element can change.
    auto const& base = navigable->active_document_url();

    // 9. Let url record be the result of applying the URL parser to url, with base URL base.
    auto url_record = URL::Parser::basic_parse(url, base);

    // 10. If url record is failure, return error with error code invalid argument.
    if (!url_record.has_value())
        return on_complete(invalid_argument("Parameter 'url' is not a valid URL"sv));

    // 11. Let request be a new request whose URL is url record.
    // 12. Return the result of await a navigation with navigable, request and wait condition.
    await_a_navigation(*navigable, url_record.release_value(), wait_condition, move(on_complete));
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-reload
void WebDriverBiDi::browsing_context_reload(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let navigable id be the value of the context field of command parameters.
    auto navigable_id = TRY_OR_COMPLETE(get_string(params, "context"sv));
    auto ignore_cache_value = TRY_OR_COMPLETE(get_optional_bool(params, "ignoreCache"sv));
    auto wait = TRY_OR_COMPLETE(get_wait_condition(params));

    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    // 3. Assert: navigable is not null.
    auto* navigable = TRY_OR_COMPLETE(get_a_navigable(navigable_id));

    // 4. Let ignore cache be the the value of the ignoreCache field of command parameters if present, or false otherwise.
    auto ignore_cache = ignore_cache_value.value_or(false);

    // 5. Let wait condition be "committed".
    auto wait_condition = WaitCondition::Committed;

    // 6. If command parameters contains wait and command parameters[wait] is not "none", set wait condition to command
    //    parameters[wait].
    if (wait == "interactive"sv)
        wait_condition = WaitCondition::Interactive;
    else if (wait == "complete"sv)
        wait_condition = WaitCondition::Complete;

    // 7. Let document be navigable's active document.
    // 8. Let url be document's URL.
    // 9. Let request be a new request whose URL is url.
    // 10. Return the result of await a navigation with navigable, request, wait condition, history handling "reload",
    //     and ignore cache ignore cache.
    // NB: The browser's UI reloads the navigable's active document by applying the reload history step, whose navigation
    //     id is passed through the history job to the replacement document.
    // NB: A child navigable is navigated to the URL of its active document, which replaces its session history entry.
    if (!navigable->is_top_level_traversable()) {
        await_a_navigation(*navigable, navigable->active_document_url(), wait_condition, move(on_complete));
        return;
    }

    auto& traversable = static_cast<CanonicalTraversable&>(*navigable);

    // NB: A reload that another reload replaces is canceled.
    if (auto previous_waiter = m_reload_waiters.take(traversable.id()); previous_waiter.has_value())
        previous_waiter->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Navigation was canceled"sv));
    auto navigation_id = Utf16String::from_utf8(generate_random_uuid());
    m_reload_waiters.set(traversable.id(), { navigation_id, wait_condition, move(on_complete) });
    traversable.view()->reload_for_webdriver_bidi(ignore_cache ? BypassCache::Yes : BypassCache::No, move(navigation_id));
}

static constexpr double MAXIMUM_DEVICE_VIEWPORT_DIMENSION = 16384;

static Gfx::IntSize device_viewport_size(ViewImplementation const& view)
{
    auto size = view.viewport_size();
    return { size.width().value(), size.height().value() };
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-setViewport
void WebDriverBiDi::browsing_context_set_viewport(JsonObject const& params, CommandResult on_complete)
{
    // browsingContext.SetViewportParameters = {
    //   ? context: browsingContext.BrowsingContext,
    //   ? viewport: browsingContext.Viewport / null,
    //   ? devicePixelRatio: (float .gt 0.0) / null,
    //   ? userContexts: [+browser.UserContext],
    // }
    // browsingContext.Viewport = {
    //   width: js-uint,
    //   height: js-uint,
    // }
    Optional<Optional<Gfx::IntSize>> viewport;
    if (auto value = params.get("viewport"sv); value.has_value()) {
        if (value->is_null()) {
            viewport = Optional<Gfx::IntSize> {};
        } else {
            if (!value->is_object())
                return on_complete(invalid_argument("Parameter 'viewport' must be a viewport or null"sv));
            auto width = value->as_object().get("width"sv);
            auto height = value->as_object().get("height"sv);
            auto width_value = width.has_value() ? as_js_uint(*width) : Optional<u64> {};
            auto height_value = height.has_value() ? as_js_uint(*height) : Optional<u64> {};
            if (!width_value.has_value() || !height_value.has_value())
                return on_complete(invalid_argument("The width and height of the viewport must be unsigned integers"sv));
            if (*width_value > NumericLimits<i32>::max() || *height_value > NumericLimits<i32>::max())
                return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "The viewport is too large"sv));
            viewport = Gfx::IntSize { static_cast<int>(*width_value), static_cast<int>(*height_value) };
        }
    }

    Optional<Optional<double>> device_pixel_ratio;
    if (auto value = params.get("devicePixelRatio"sv); value.has_value()) {
        if (value->is_null()) {
            device_pixel_ratio = Optional<double> {};
        } else {
            auto number = value->get_number_with_precision_loss<double>();
            if (!number.has_value() || !(*number > 0))
                return on_complete(invalid_argument("Parameter 'devicePixelRatio' must be a number greater than 0"sv));
            device_pixel_ratio = number;
        }
    }

    auto context = TRY_OR_COMPLETE(get_optional_string(params, "context"sv));
    auto user_contexts = TRY_OR_COMPLETE(get_optional_non_empty_list_of_strings(params, "userContexts"sv));

    // 1. If the implementation is unable to adjust the layout viewport parameters with the given command parameters for
    //    any reason, return error with error code unsupported operation.

    // 2. If command parameters contains "userContexts" and command parameters contains "context", return error with
    //    error code invalid argument.
    if (context.has_value() && user_contexts.has_value())
        return on_complete(invalid_argument("Parameters 'context' and 'userContexts' are mutually exclusive"sv));

    // 3. Let navigables be a set.
    Vector<CanonicalTraversable*> navigables;

    // 4. If the context field of command parameters is present:
    if (context.has_value()) {
        // 1. Let navigable id be the value of the context field of command parameters.
        // 2. Let navigable be the result of trying to get a navigable with navigable id.
        // 3. If navigable is not a top-level traversable, return error with error code invalid argument.
        // 4. Append navigable to navigables.
        navigables.append(TRY_OR_COMPLETE(get_a_top_level_traversable(*context)));
    }
    // 5. Otherwise, if the userContexts field of command parameters is present:
    else if (user_contexts.has_value()) {
        // 1. Let user contexts be the result of trying to get valid user contexts with command
        //    parameters["userContexts"].
        for (auto const& user_context_id : *user_contexts) {
            if (user_context_id != DEFAULT_USER_CONTEXT_ID)
                return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchUserContext, "User context not found"sv));
        }

        // 2. For each user context of user contexts:
        //     1. Set viewport overrides map[user context] to a struct.
        //     2. If command parameters contains "viewport":
        //         1. Set viewport overrides map[user context]'s viewport to command parameters["viewport"].
        //     3. If command parameters contains "devicePixelRatio":
        //         1. Set viewport overrides map[user context]'s devicePixelRatio to command parameters["devicePixelRatio"].
        //     4. For each top-level traversable of the list of all top-level traversables whose associated user context
        //        is user context:
        //         1. Append top-level traversable to navigables.
        // FIXME: Apply the overrides of a user context to the top-level traversables created after this.
        for (auto& traversable : all_top_level_traversables())
            navigables.append(&traversable);
    }
    // 6. Otherwise, return error with error code invalid argument.
    else {
        return on_complete(invalid_argument("Either parameter 'context' or 'userContexts' must be given"sv));
    }

    struct ViewportChange {
        WeakPtr<ViewImplementation> view;
        String handle;
        ViewportDefaults defaults;
        double ratio;
        Gfx::IntSize device_viewport;
        Optional<Gfx::IntSize> css_override;
    };
    Vector<ViewportChange> changes;
    for (auto* navigable : navigables) {
        auto view = navigable->view();
        if (!view.has_value())
            continue;

        // NB: The layout viewport of a top-level traversable is the viewport of its view, which a headless view takes
        //     the size of its window for. The view remembers the size it had before the first override, which is its
        //     implementation-defined default.
        auto defaults = m_viewport_defaults.get(view->handle()).value_or(ViewportDefaults { .viewport_size = device_viewport_size(*view), .device_pixel_ratio = view->device_pixel_ratio() });

        auto current_ratio = view->device_pixel_ratio();
        Optional<Gfx::IntSize> css_override;
        if (auto saved = m_css_viewport_overrides.get(view->handle()); saved.has_value() && saved->device_size == device_viewport_size(*view))
            css_override = saved->css_size;
        auto css_viewport = css_override.value_or(device_viewport_size(*view).to_type<double>().scaled(1.0 / current_ratio).to_type<int>());

        // 8. If command parameters contains the devicePixelRatio field:
        //     1. Let device pixel ratio be the command parameters["devicePixelRatio"].
        //     2. For each navigable of navigables:
        //         1. For the navigable and all descendant navigables:
        //             1. Set device pixel ratio override with navigable and device pixel ratio.
        auto ratio = current_ratio;
        if (device_pixel_ratio.has_value())
            ratio = device_pixel_ratio->value_or(defaults.device_pixel_ratio);

        // 7. If command parameters contains the viewport field:
        //     1. Let viewport be the command parameters["viewport"].
        //     2. For each navigable of navigables:
        //         1. Set viewport with navigable and viewport.
        //         2. Run the resizing viewports steps with navigable's active document.
        if (viewport.has_value()) {
            css_override = *viewport;
            css_viewport = viewport->value_or(defaults.viewport_size.to_type<double>().scaled(1.0 / defaults.device_pixel_ratio).to_type<int>());
        }

        // NB: This is step 1 for a viewport larger than the bitmaps that back the viewport of a view can be.
        auto device_viewport = css_viewport.to_type<double>().scaled(ratio);
        if (!(device_viewport.width() <= MAXIMUM_DEVICE_VIEWPORT_DIMENSION) || !(device_viewport.height() <= MAXIMUM_DEVICE_VIEWPORT_DIMENSION))
            return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "The viewport is too large"sv));

        // NB: Round up so conversion back to CSS pixels does not reduce the requested integer viewport.
        Gfx::IntSize rounded_device_viewport { static_cast<int>(ceil(device_viewport.width())), static_cast<int>(ceil(device_viewport.height())) };
        changes.append({ view->make_weak_ptr(), view->handle(), defaults, ratio, rounded_device_viewport, css_override });
    }

    // NB: A failed command must leave both the views and their remembered defaults unchanged. Cache all defaults
    //     before calling resize callbacks, which may destroy another view or reenter viewport handling.
    for (auto const& change : changes) {
        if (change.view) {
            m_viewport_defaults.ensure(change.handle, [&] { return change.defaults; });
            if (change.css_override.has_value())
                m_css_viewport_overrides.set(change.handle, { *change.css_override, change.device_viewport });
            else
                m_css_viewport_overrides.remove(change.handle);
        }
    }
    for (auto const& change : changes) {
        auto* view = change.view.ptr();
        if (!view)
            continue;
        if (change.ratio != view->device_pixel_ratio())
            view->set_device_pixel_ratio_for_testing(view->page(), change.ratio);
        if (view->on_resize_window)
            view->on_resize_window(change.device_viewport);
    }

    // 9. Return success with data null.
    on_complete(JsonValue {});
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-traverseHistory
void WebDriverBiDi::browsing_context_traverse_history(JsonObject const& params, CommandResult on_complete)
{
    auto navigable_id = TRY_OR_COMPLETE(get_string(params, "context"sv));

    // browsingContext.TraverseHistoryParameters = {
    //   context: browsingContext.BrowsingContext,
    //   delta: js-int,
    // }
    // js-int = -9007199254740991..9007199254740991
    auto delta_value = params.get("delta"sv);
    auto delta = delta_value.has_value() ? delta_value->get_number_with_precision_loss<double>() : Optional<double> {};
    if (!delta.has_value() || trunc(*delta) != *delta || fabs(*delta) > 9007199254740991.0)
        return on_complete(invalid_argument("Parameter 'delta' must be an integer"sv));

    // 1. Let navigable be the result of trying to get a navigable with command parameters["context"].
    // 2. If navigable is not a top-level traversable, return error with error code invalid argument.
    // 3. Assert: navigable is not null.
    auto* traversable = TRY_OR_COMPLETE(get_a_top_level_traversable(navigable_id));

    // 4. Let delta be command parameters["delta"].
    // 5. Let resume id be a unique string.
    // 6. Queue a task on navigable's session history traversal queue to run the following steps:
    //     1. Let all steps be the result of getting all used history steps for navigable.
    //     2. Let current index be the index of navigable's current session history step within all steps.
    //     3. Let target index be current index plus delta.
    //     4. Let valid entry be false if all steps[target index] does not exist, or true otherwise.
    //     5. Resume with "check history", resume id, and valid entry.
    // 7. Let is valid entry be await with «"check history"», and resume id.
    // NB: The browser process holds the session history, so it checks the entry right away.
    auto all_steps = traversable->session_history().used_steps();
    auto current_index = all_steps.find_first_index(traversable->session_history().current_step());
    auto target_index = current_index.has_value() ? static_cast<double>(*current_index) + *delta : -1.0;
    auto is_valid_entry = target_index >= 0 && target_index < static_cast<double>(all_steps.size());

    // 8. If is valid entry is false, return error with error code no such history entry.
    if (!is_valid_entry)
        return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchHistoryEntry, "No history entry at the given delta"sv));

    // 9. Traverse the history by a delta given delta and navigable.
    // 10. TODO: Support waiting for the history traversal to complete.
    // 11. Let body be a map matching the browsingContext.TraverseHistoryResult production.
    // 12. Return success with data body.
    // NB: The command completes once the traversal is applied.
    // NB: A traversal that is canceled or that does not apply completes the command with an error.
    struct PendingTraversal : RefCounted<PendingTraversal> {
        explicit PendingTraversal(CommandResult on_complete)
            : on_complete(move(on_complete))
        {
        }
        ~PendingTraversal()
        {
            if (on_complete)
                on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "History traversal did not complete"sv));
        }
        CommandResult on_complete;
    };
    auto pending_traversal = adopt_ref(*new PendingTraversal(move(on_complete)));
    traversable->view()->traverse_the_history_by_delta(static_cast<int>(*delta), CheckForCancelation::Yes, [pending_traversal] {
        if (auto on_complete = move(pending_traversal->on_complete))
            on_complete(JsonValue {});
    });
}

// https://w3c.github.io/webdriver-bidi/#realm-id
// NB: The id of a realm names the document whose relevant realm it is, which the process hosting it gave.
static Optional<Web::HTML::CrossProcessId> document_id_of_realm_id(StringView realm_id)
{
    auto parts = realm_id.split_view('.');
    if (parts.size() != 2)
        return {};

    auto namespace_id = parts[0].to_number<u64>();
    auto local_id = parts[1].to_number<u64>();
    if (!namespace_id.has_value() || !local_id.has_value())
        return {};

    return Web::HTML::CrossProcessId { .namespace_id = *namespace_id, .local_id = *local_id };
}

// https://w3c.github.io/webdriver-bidi/#get-a-realm
static ErrorOr<CanonicalNavigable*, Web::WebDriver::Error> get_the_navigable_of_a_realm(StringView realm_id)
{
    // 1. If realm id is null, return success with data null.
    // 2. If there is no realm with id realm id return error with error code no such frame
    auto document_id = document_id_of_realm_id(realm_id);
    if (!document_id.has_value())
        return no_such_frame();

    CanonicalNavigable* result = nullptr;
    for (auto& traversable : all_top_level_traversables()) {
        traversable.for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
            if (navigable.active_document().id() != *document_id)
                return IterationDecision::Continue;
            result = &navigable;
            return IterationDecision::Break;
        });
        if (result)
            break;
    }

    if (!result)
        return no_such_frame();

    // 3. Let realm be the realm with id realm id.
    // 4. Return success with data realm
    return result;
}

// https://w3c.github.io/webdriver-bidi/#get-a-realm-from-a-target
void WebDriverBiDi::route_to_script_target(String const& method, JsonObject const& params, CommandResult on_complete)
{
    auto target = params.get("target"sv);
    if (!target.has_value() || !target->is_object())
        return on_complete(invalid_argument("Parameter 'target' must be an object"sv));

    // 1. If target matches the script.ContextTarget production:
    if (target->as_object().has("context"sv)) {
        // 1. Let sandbox be null.
        // 2. If target contains "sandbox", set sandbox to target["sandbox"].
        // 3. Let realm be get a realm from a navigable with target["context"] and sandbox.
        // https://w3c.github.io/webdriver-bidi/#get-a-realm-from-a-navigable
        // 1. Let navigable be the result of trying to get a navigable with navigable id.
        // NB: The process hosting the navigable's active document gets its realm.
        auto navigable_id = TRY_OR_COMPLETE(get_string(target->as_object(), "context"sv));
        auto* navigable = TRY_OR_COMPLETE(get_a_navigable(navigable_id));
        run_content_command(*navigable, method, params, move(on_complete));
        return;
    }

    // 2. Otherwise:
    if (target->as_object().has("realm"sv)) {
        // 1. Assert: target matches the script.RealmTarget production.
        // 2. Let realm id be the value of the realm field of target.
        auto realm_id = TRY_OR_COMPLETE(get_string(target->as_object(), "realm"sv));

        // 3. Let realm be get a realm given realm id.
        // NB: The process hosting the realm's document gets it.
        auto* navigable = TRY_OR_COMPLETE(get_the_navigable_of_a_realm(realm_id));
        run_content_command(*navigable, method, params, move(on_complete));
        return;
    }

    on_complete(invalid_argument("Parameter 'target' must be a context or realm target"sv));
}

// https://w3c.github.io/webdriver-bidi/#command-script-getRealms
void WebDriverBiDi::script_get_realms(JsonObject const& params, CommandResult on_complete)
{
    auto context = TRY_OR_COMPLETE(get_optional_string(params, "context"sv));
    auto type = TRY_OR_COMPLETE(get_optional_string(params, "type"sv));

    // script.RealmType = "window" / "dedicated-worker" / "shared-worker" / "service-worker" / "worker" /
    //                    "paint-worklet" / "audio-worklet" / "worklet"
    if (type.has_value() && !type->is_one_of("window"sv, "dedicated-worker"sv, "shared-worker"sv, "service-worker"sv, "worker"sv, "paint-worklet"sv, "audio-worklet"sv, "worklet"sv))
        return on_complete(invalid_argument("Parameter 'type' must be a realm type"sv));

    // 1. Let environment settings be a list of all the environment settings objects that have their execution ready
    //    flag set.
    // 2. If command parameters contains context:
    //     1. Let navigable be the result of trying to get a navigable with command parameters["context"].
    // NB: The processes hosting the navigables' documents hold their realms.
    Vector<CanonicalNavigable const*> navigables;
    if (context.has_value()) {
        navigables.append(TRY_OR_COMPLETE(get_a_navigable(*context)));
    } else {
        for (auto& traversable : all_top_level_traversables()) {
            traversable.for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
                navigables.append(&navigable);
                return IterationDecision::Continue;
            });
        }
    }

    struct State : RefCounted<State> {
        Vector<Optional<JsonArray>> realms;
        size_t remaining { 0 };
        CommandResult on_complete;
    };
    auto state = adopt_ref(*new State);
    state->realms.resize(navigables.size());
    state->remaining = navigables.size();
    state->on_complete = move(on_complete);

    auto finish = [state] {
        // 3. Let realms be a list.
        JsonArray realms;
        for (auto& navigable_realms : state->realms) {
            if (!navigable_realms.has_value())
                continue;
            for (auto& realm : navigable_realms->values())
                realms.must_append(realm);
        }

        // 5. Let body be a map matching the script.GetRealmsResult production, with the realms field set to realms.
        JsonObject body;
        body.set("realms"sv, move(realms));

        // 6. Return success with data body.
        state->on_complete(JsonValue { move(body) });
    };

    if (navigables.is_empty()) {
        finish();
        return;
    }

    for (size_t i = 0; i < navigables.size(); ++i) {
        JsonObject navigable_params;
        navigable_params.set("context"sv, navigable_id(*navigables[i]));
        if (type.has_value())
            navigable_params.set("type"sv, *type);

        run_content_command(*navigables[i], "script.getRealms"_string, move(navigable_params), [state, i, finish, single_navigable = context.has_value()](Web::WebDriver::Response response) {
            if (response.is_error()) {
                // NB: A navigable that went away while we asked has no realms to report.
                if (single_navigable) {
                    state->on_complete(move(response));
                    return;
                }
            } else if (response.value().is_object()) {
                if (auto realms = response.value().as_object().get_array("realms"sv); realms.has_value())
                    state->realms[i] = *realms;
            }
            if (--state->remaining == 0)
                finish();
        });
    }
}

// https://w3c.github.io/webdriver-bidi/#type-script-ChannelValue
static bool is_channel_value(JsonValue const& value)
{
    // script.ChannelValue = {
    //   type: "channel",
    //   value: script.ChannelProperties,
    // }
    // script.ChannelProperties = {
    //   channel: script.Channel,
    //   ? serializationOptions: script.SerializationOptions,
    //   ? ownership: script.ResultOwnership,
    // }
    if (!value.is_object())
        return false;
    auto const& object = value.as_object();
    if (object.get_string("type"sv) != "channel"sv)
        return false;
    auto properties = object.get_object("value"sv);
    if (!properties.has_value() || !properties->get_string("channel"sv).has_value())
        return false;
    if (auto ownership = properties->get("ownership"sv); ownership.has_value() && (!ownership->is_string() || !ownership->as_string().is_one_of("root"sv, "none"sv)))
        return false;
    if (auto options = properties->get("serializationOptions"sv); options.has_value() && Web::WebDriver::parse_serialization_options(*options).is_error())
        return false;
    return true;
}

// https://w3c.github.io/webdriver-bidi/#command-script-addPreloadScript
void WebDriverBiDi::script_add_preload_script(JsonObject const& params, CommandResult on_complete)
{
    // 1. If command parameters contains "userContexts" and command parameters contains "contexts", return error with
    //    error code invalid argument.
    if (params.has("userContexts"sv) && params.has("contexts"sv))
        return on_complete(invalid_argument("Parameters 'contexts' and 'userContexts' are mutually exclusive"sv));

    // 2. Let function declaration be the functionDeclaration field of command parameters.
    auto function_declaration = TRY_OR_COMPLETE(get_string(params, "functionDeclaration"sv));

    // 3. Let arguments be the arguments field of command parameters if present, or an empty list otherwise.
    JsonArray arguments;
    if (auto value = params.get("arguments"sv); value.has_value()) {
        if (!value->is_array())
            return on_complete(invalid_argument("Parameter 'arguments' must be a list of channel values"sv));
        for (auto const& argument : value->as_array().values()) {
            if (!is_channel_value(argument))
                return on_complete(invalid_argument("Parameter 'arguments' must be a list of channel values"sv));
            arguments.must_append(argument);
        }
    }

    auto contexts = TRY_OR_COMPLETE(get_optional_non_empty_list_of_strings(params, "contexts"sv));
    auto input_user_contexts = TRY_OR_COMPLETE(get_optional_non_empty_list_of_strings(params, "userContexts"sv));
    auto sandbox = TRY_OR_COMPLETE(get_optional_string(params, "sandbox"sv));

    // 4. Let user contexts to be a set.
    Vector<String> user_contexts;

    // 5. Let navigables be null.
    Optional<Vector<String>> navigables;

    // 6. If the contexts field of command parameters is present:
    if (contexts.has_value()) {
        // 1. Set navigables to an empty set.
        navigables = Vector<String> {};

        // 2. For each navigable id of command parameters["contexts"]
        for (auto const& navigable_id : *contexts) {
            // 1. Let navigable be the result of trying to get a navigable with navigable id.
            // 2. If navigable is not a top-level traversable, return error with error code invalid argument.
            auto* traversable = TRY_OR_COMPLETE(get_a_top_level_traversable(navigable_id));

            // 3. Append navigable to navigables.
            if (!navigables->contains_slow(navigable_id))
                navigables->append(navigable_id);

            // NB: A process that hosts a frame of the traversable, and not the traversable itself, names it by its id.
            auto traversable_id = MUST(String::formatted("{}-{}", traversable->id().namespace_id, traversable->id().local_id));
            if (!navigables->contains_slow(traversable_id))
                navigables->append(move(traversable_id));
        }
    }
    // 7. Otherwise, if command parameters contains userContexts:
    else if (input_user_contexts.has_value()) {
        // 1. Set user contexts to create a set with command parameters["userContexts"].
        for (auto const& user_context_id : *input_user_contexts) {
            if (!user_contexts.contains_slow(user_context_id))
                user_contexts.append(user_context_id);
        }

        // 2. For each user context id of user contexts:
        for (auto const& user_context_id : user_contexts) {
            // 1. Set user context to get user context with user context id.
            // 2. If user context is null, return error with error code no such user context.
            if (user_context_id != DEFAULT_USER_CONTEXT_ID)
                return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchUserContext, "User context not found"sv));
        }
    }

    // 8. Let sandbox be the value of the "sandbox" field in command parameters, if present, or null otherwise.

    // 9. Let script be the string representation of a UUID.
    auto script = generate_random_uuid();

    // 10. Let preload script map be session's preload script map.
    // 11. Set preload script map[script] to a struct with function declaration function declaration, arguments
    //     arguments, contexts navigables, sandbox sandbox, and user contexts user contexts.
    m_session_state.preload_scripts.append({
        .id = script,
        .function_declaration = move(function_declaration),
        .arguments = move(arguments),
        .contexts = move(navigables),
        .sandbox = move(sandbox),
        .user_contexts = move(user_contexts),
    });
    push_session_state();

    // 12. Return a new map matching the script.AddPreloadScriptResult with the script field set to script.
    JsonObject result;
    result.set("script"sv, move(script));
    on_complete(JsonValue { move(result) });
}

// https://w3c.github.io/webdriver-bidi/#command-script-removePreloadScript
void WebDriverBiDi::script_remove_preload_script(JsonObject const& params, CommandResult on_complete)
{
    // 1. Let script be the value of the "script" field in command parameters.
    auto script = TRY_OR_COMPLETE(get_string(params, "script"sv));

    // 2. Let preload script map be session's preload script map.
    // 3. If preload script map does not contain script, return error with error code no such script.
    // 4. Remove script from preload script map.
    auto removed = m_session_state.preload_scripts.remove_first_matching([&](auto const& preload_script) {
        return preload_script.id == script;
    });
    if (!removed)
        return on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchScript, "Preload script not found"sv));

    push_session_state();

    // 5. Return null
    on_complete(JsonValue {});
}

}
