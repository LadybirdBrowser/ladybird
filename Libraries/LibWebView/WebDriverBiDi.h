/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibGfx/Size.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/WebDriver/BiDi.h>
#include <LibWebCommon/WebDriver/Response.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WebDriverBrowserConnection;
class WebDriverBiDiNetwork;

// https://w3c.github.io/webdriver-bidi/
// NB: The browser process runs the commands of a BiDi session that involve navigables, and routes those that run in the
//     process hosting a navigable's document to it. It exists only while WebDriver drives the browser.
class WebDriverBiDi : public Weakable<WebDriverBiDi> {
    AK_MAKE_NONCOPYABLE(WebDriverBiDi);
    AK_ALLOC_WITH_KMALLOC;
    AK_MAKE_NONMOVABLE(WebDriverBiDi);

public:
    static WebDriverBiDi* the() { return s_the; }

    explicit WebDriverBiDi(WebDriverBrowserConnection&);
    ~WebDriverBiDi();

    void run_command(u64 command_id, String const& method, JsonValue params);
    void get_top_level_traversables(u64 command_id, Vector<String> const& navigable_ids);
    void run_subscribe_steps(u64 command_id, String const& event_name, Vector<String> const& top_level_traversable_ids);
    void set_enabled_events(Vector<String> const& event_names);

    bool is_event_enabled(StringView event_name) const { return m_enabled_events.contains(event_name); }
    Web::WebDriver::BiDiSessionState const& session_state() const { return m_session_state; }

    // Completes a command that a process hosting a navigable ran, if this object sent it.
    bool did_complete_content_command(u64 command_id, Web::WebDriver::Response&);

    // https://w3c.github.io/webdriver-bidi/#navigable-id
    static String navigable_id(CanonicalNavigable const&);

    // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigable-created
    void navigable_created(CanonicalNavigable const&, Optional<CanonicalNavigable const&> opener_navigable = {});
    void top_level_traversable_did_get_window_handle(CanonicalTraversable const&);

    // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigable-destroyed
    void navigable_destroyed(CanonicalNavigable const&);
    void top_level_traversable_is_closing(CanonicalNavigable const&);
    void top_level_traversable_did_close(String const& window_handle);

    // NB: The navigations of a top-level traversable whose process crashed do not go on.
    void top_level_traversable_did_crash(CanonicalTraversable const&);

    // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-status
    enum class NavigationStatusType : u8 {
        Canceled,
        Pending,
        Complete,
    };
    struct NavigationStatus {
        Optional<Utf16String> id;
        NavigationStatusType status { NavigationStatusType::Pending };
        URL::URL url;
    };

    // NB: A reload canceled before its history step applies emits no navigation event.
    void reload_did_not_apply(CanonicalTraversable const&, Optional<Utf16String> const& navigation_id);
    void navigation_superseded_reload(CanonicalNavigable const&);

    // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-started
    void navigation_started(CanonicalNavigable const&, NavigationStatus const&);

    // https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-failed
    void navigation_failed(CanonicalNavigable const&, NavigationStatus const&);

    void did_receive_navigation_event(WebContentPage&, Web::HTML::CrossProcessId navigable_id, String const& name, Optional<Utf16String> navigation_id, URL::URL url);
    void download_response_finished(String const& context,
        JsonObject const& metadata,
        u64 decoded_body_size,
        Requests::RequestTimingInfo const*,
        Optional<StringView> error);
    void did_receive_event(WebContentPage&, Web::HTML::CrossProcessId navigable_id, String const& name, JsonValue params);

private:
    static WebDriverBiDi* s_the;

    using CommandResult = Function<void(Web::WebDriver::Response)>;

    void complete_command(u64 command_id, Web::WebDriver::Response);

    void emit_event(StringView name, JsonValue params, Vector<String> top_level_traversable_ids);
    void emit_event_for_navigable(StringView name, JsonValue params, CanonicalNavigable const&);

    void ensure_session_is_active();
    void push_session_state();

    void run_content_command(CanonicalNavigable const&, String const& method, JsonValue params, CommandResult);

    // https://w3c.github.io/webdriver-bidi/#get-the-navigable-info
    JsonObject get_the_navigable_info(CanonicalNavigable const&, Optional<u64> max_depth, bool include_parent_id) const;

    // https://w3c.github.io/webdriver-bidi/#get-the-navigation-info
    JsonObject get_the_navigation_info(CanonicalNavigable const&, NavigationStatus const&) const;

    // https://w3c.github.io/webdriver-bidi/#await
    using ResumeSteps = Function<void(StringView event_name, NavigationStatus)>;
    // NB: A navigation is awaited for the navigable it navigates, whose events alone resume it.
    void await(Vector<StringView> events, Web::HTML::CrossProcessId navigable_id, Utf16String const& resume_id, ResumeSteps);

    // https://w3c.github.io/webdriver-bidi/#resume
    void resume(StringView name, Web::HTML::CrossProcessId navigable_id, Utf16String const& id, NavigationStatus const&);

    // NB: The navigations awaited for a navigable that is gone, or whose process is gone, fail.
    void fail_navigations_of(Function<bool(Web::HTML::CrossProcessId)> const& is_affected_navigable);
    void remove_state_of_removed_navigables();

    // https://w3c.github.io/webdriver-bidi/#await-a-navigation
    enum class WaitCondition : u8 {
        Committed,
        Interactive,
        Complete,
    };
    void await_a_navigation(CanonicalNavigable&, URL::URL, WaitCondition, CommandResult);
    void await_navigation_completion(Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_id, WaitCondition, CommandResult);

    void browsing_context_activate(JsonObject const& params, CommandResult);
    void browsing_context_close(JsonObject const& params, CommandResult);
    void browsing_context_create(JsonObject const& params, CommandResult);
    void browsing_context_get_tree(JsonObject const& params, CommandResult);
    void browsing_context_navigate(JsonObject const& params, CommandResult);
    void browsing_context_reload(JsonObject const& params, CommandResult);
    void browsing_context_set_viewport(JsonObject const& params, CommandResult);
    void browsing_context_traverse_history(JsonObject const& params, CommandResult);
    void route_to_navigable(String const& method, JsonObject const& params, CommandResult);
    String os_window_of(String const& top_level_traversable_id) const;

    void script_add_preload_script(JsonObject const& params, CommandResult);
    void script_remove_preload_script(JsonObject const& params, CommandResult);
    void script_get_realms(JsonObject const& params, CommandResult);
    void route_to_script_target(String const& method, JsonObject const& params, CommandResult);

    WebDriverBrowserConnection& m_connection;

    HashTable<String> m_enabled_events;
    Web::WebDriver::BiDiSessionState m_session_state;

    OwnPtr<WebDriverBiDiNetwork> m_network;

    u64 m_next_content_command_id;
    HashMap<u64, CommandResult> m_pending_content_commands;

    // https://w3c.github.io/webdriver-bidi/#wait-queue
    struct Waiter {
        Vector<StringView> events;
        Web::HTML::CrossProcessId navigable_id;
        ResumeSteps algorithm;
    };
    HashMap<Utf16String, Waiter> m_wait_queue;

    // https://w3c.github.io/webdriver-bidi/#original-opener
    HashMap<Web::HTML::CrossProcessId, String> m_original_openers;

    // NB: The navigables whose destruction was reported before they were removed.
    HashTable<Web::HTML::CrossProcessId> m_destroyed_navigables;

    // NB: The info of each closing top-level traversable, taken before its navigables go away with it.
    HashMap<Web::HTML::CrossProcessId, JsonObject> m_closing_traversable_infos;

    // NB: The commands closing a top-level traversable complete once it is closed.
    HashMap<String, Vector<CommandResult>> m_pending_close_commands;

    // NB: Reload commands wait for events from their own navigation.
    struct ReloadWaiter {
        Utf16String navigation_id;
        WaitCondition wait_condition;
        CommandResult on_complete;
    };
    HashMap<Web::HTML::CrossProcessId, ReloadWaiter> m_reload_waiters;

    // NB: The viewport a top-level traversable had before WebDriver BiDi first overrode it.
    struct ViewportDefaults {
        Gfx::IntSize viewport_size;
        double device_pixel_ratio { 1.0 };
    };
    HashMap<String, ViewportDefaults> m_viewport_defaults;
    struct ViewportOverride {
        Gfx::IntSize css_size;
        Gfx::IntSize device_size;
    };
    HashMap<String, ViewportOverride> m_css_viewport_overrides;

    // NB: The OS window of each tab created in the OS window of another top-level traversable.
    HashMap<String, String> m_tab_windows;
};

}
