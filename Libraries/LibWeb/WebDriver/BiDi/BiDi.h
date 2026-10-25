/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/kmalloc.h>
#include <LibJS/Console.h>
#include <LibJS/Forward.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebDriver/BiDi.h>

// https://w3c.github.io/webdriver-bidi/
// NB: The parts of a BiDi session that run in the process hosting a navigable's document. A process only does any of
//     this work while the browser process says a BiDi session is active, which only WebDriver can start.
namespace Web::WebDriver::BiDi {

WEB_API extern bool g_is_active;

// Whether a BiDi session is active.
inline bool is_active() { return g_is_active; }

WEB_API void set_session_state(BiDiSessionState);
WEB_API BiDiSessionState const& session_state();
WEB_API bool is_event_enabled(StringView event_name);

// Where this process reports the events of the navigables it hosts.
struct WEB_API EventSink {
    AK_ALLOC_WITH_KMALLOC;

    Function<void(HTML::Navigable const&, String const& name, JsonValue params)> on_event;
    Function<void(HTML::Navigable const&, StringView name, Optional<Utf16String> const& navigation_id, URL::URL const& url)> on_navigation_event;
};
WEB_API void set_event_sink(EventSink);

// https://w3c.github.io/webdriver-bidi/#navigable-id
WEB_API String navigable_id(HTML::Navigable const&);

// https://w3c.github.io/webdriver-bidi/#emit-an-event
// NB: The browser process emits the event for the sessions that enable it given the navigable it relates to.
void emit_event(HTML::Navigable const&, String name, JsonValue params);

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-navigation-status
// NB: Reports the event that the WebDriver BiDi navigation status steps of a navigable emit, such as
//     "browsingContext.load". The browser process emits it.
void navigation_event(HTML::Navigable const&, StringView event_name, Optional<Utf16String> const& navigation_id, URL::URL const&);

// NB: The steps to run once a document is active in its navigable and before scripts may run for it: the document's
//     realm is created as far as WebDriver BiDi can tell, and the preload scripts run in it.
WEB_API void document_is_ready_for_scripts(DOM::Document&);

// https://w3c.github.io/webdriver-bidi/#event-script-realmDestroyed
void unloading_document_cleanup_steps(DOM::Document&);

// https://w3c.github.io/webdriver-bidi/#event-log-entryAdded
WEB_API void console_steps(HTML::EnvironmentSettingsObject&, JS::Console::LogLevel, JS::ConsoleClient::PrinterArguments const&);
void error_reporting_steps(HTML::EnvironmentSettingsObject&, JS::Value exception);

// NB: A realm whose console has no client of its own gets one that only runs the console steps.
void ensure_console_steps_run_for(DOM::Document&);

}
