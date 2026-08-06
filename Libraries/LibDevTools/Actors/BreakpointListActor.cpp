/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/BreakpointListActor.h>
#include <LibDevTools/Actors/SourceActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<BreakpointListActor> BreakpointListActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new BreakpointListActor(devtools, move(name), move(tab)));
}

BreakpointListActor::BreakpointListActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

BreakpointListActor::~BreakpointListActor() = default;

void BreakpointListActor::handle_message(Message const& message)
{
    if (message.type == "setBreakpoint"sv || message.type == "removeBreakpoint"sv) {
        auto location = breakpoint_location(message);
        if (!location.has_value())
            return;

        auto tab = m_tab.strong_ref();
        if (!tab) {
            send_breakpoint_error(message, Error::from_string_literal("Unable to locate tab"));
            return;
        }

        if (message.type == "setBreakpoint"sv) {
            WebView::DebuggerBreakpointOptions options;
            if (auto options_object = message.data.get_object("options"sv); options_object.has_value()) {
                if (auto condition = options_object->get_string("condition"sv); condition.has_value())
                    options.condition = Utf16String::from_utf8(*condition);
                if (auto log_value = options_object->get_string("logValue"sv); log_value.has_value())
                    options.log_value = Utf16String::from_utf8(*log_value);
                options.show_stacktrace = options_object->get_bool("showStacktrace"sv).value_or(false);
            }
            auto breakpoint_location = location.release_value();
            auto retained_location = breakpoint_location;
            auto retained_options = options;
            devtools().delegate().set_debugger_breakpoint(tab->description(), move(breakpoint_location), move(options),
                [weak_self = make_weak_ptr<BreakpointListActor>(), message_id = message.id, breakpoint_location = move(retained_location), options = move(retained_options)](ErrorOr<void> result) mutable {
                    auto self = weak_self.strong_ref();
                    if (!self)
                        return;
                    if (result.is_error()) {
                        self->send_breakpoint_error({ .id = message_id }, result.error());
                        return;
                    }
                    self->remember_breakpoint(move(breakpoint_location), move(options));
                    self->send_response({ .id = message_id }, {});
                });
        } else {
            auto breakpoint_location = location.release_value();
            if (auto breakpoint = m_breakpoints.find_if([&](auto const& breakpoint) { return breakpoint.location.represents_same_breakpoint_as(breakpoint_location); }); breakpoint != m_breakpoints.end())
                breakpoint_location = breakpoint->location;
            auto retained_location = breakpoint_location;
            devtools().delegate().remove_debugger_breakpoint(tab->description(), move(breakpoint_location),
                [weak_self = make_weak_ptr<BreakpointListActor>(), message_id = message.id, breakpoint_location = move(retained_location)](ErrorOr<void> result) mutable {
                    auto self = weak_self.strong_ref();
                    if (!self)
                        return;
                    if (result.is_error()) {
                        self->send_breakpoint_error({ .id = message_id }, result.error());
                        return;
                    }
                    self->forget_breakpoint(breakpoint_location);
                    self->send_response({ .id = message_id }, {});
                });
        }
        return;
    }

    if (message.type == "setXHRBreakpoint"sv || message.type == "removeXHRBreakpoint"sv || message.type == "setActiveEventBreakpoints"sv) {
        // FIXME: Implement XHR and event breakpoints.
        send_response(message, {});
        return;
    }

    send_unrecognized_packet_type_error(message);
}

Optional<WebView::DebuggerBreakpointLocation> BreakpointListActor::breakpoint_location(Message const& message)
{
    auto location = get_required_parameter<JsonObject>(message, "location"sv);
    if (!location.has_value())
        return {};

    auto line = location->get_integer<u32>("line"sv);
    if (!line.has_value()) {
        send_missing_parameter_error(message, "location.line"sv);
        return {};
    }

    Optional<u32> column;
    if (auto column_value = location->get("column"sv); column_value.has_value() && !column_value->is_null()) {
        column = column_value->get_integer<u32>();
        if (!column.has_value()) {
            send_missing_parameter_error(message, "location.column"sv);
            return {};
        }
    }

    auto source_id = location->get_string("sourceId"sv);
    if (source_id.has_value()) {
        auto source_actor = devtools().actor_registry().find(*source_id);
        if (source_actor != devtools().actor_registry().end() && is<SourceActor>(*source_actor->value)) {
            return WebView::DebuggerBreakpointLocation {
                .source_id = static_cast<SourceActor&>(*source_actor->value).source().id,
                .filename = static_cast<SourceActor&>(*source_actor->value).source().display_url,
                .line = *line,
                .column = column,
            };
        }
    }

    if (auto source_url = location->get_string("sourceUrl"sv); source_url.has_value()) {
        return WebView::DebuggerBreakpointLocation {
            .source_id = {},
            .filename = Utf16String::from_utf8(*source_url),
            .line = *line,
            .column = column,
        };
    }

    if (source_id.has_value()) {
        send_unknown_actor_error(message, *source_id);
        return {};
    }

    send_missing_parameter_error(message, "location.sourceUrl"sv);
    return {};
}

void BreakpointListActor::remember_breakpoint(WebView::DebuggerBreakpointLocation location, WebView::DebuggerBreakpointOptions options)
{
    if (auto breakpoint = m_breakpoints.find_if([&](auto const& breakpoint) { return breakpoint.location.represents_same_breakpoint_as(location); }); breakpoint != m_breakpoints.end()) {
        breakpoint->options = move(options);
        return;
    }
    m_breakpoints.append({ move(location), move(options) });
}

void BreakpointListActor::forget_breakpoint(WebView::DebuggerBreakpointLocation const& location)
{
    m_breakpoints.remove_first_matching([&](auto const& breakpoint) { return breakpoint.location.represents_same_breakpoint_as(location); });
}

void BreakpointListActor::reapply_breakpoints()
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    for (auto const& breakpoint : m_breakpoints) {
        auto previous_location = breakpoint.location;
        devtools().delegate().remove_debugger_breakpoint(tab->description(), breakpoint.location,
            [weak_self = make_weak_ptr<BreakpointListActor>(), previous_location = move(previous_location)](ErrorOr<void> removal_result) mutable {
                auto self = weak_self.strong_ref();
                if (!self)
                    return;

                if (removal_result.is_error())
                    dbgln_if(DEVTOOLS_DEBUG, "Unable to remove debugger breakpoint before reapplying it: {}", removal_result.error());

                auto breakpoint = self->m_breakpoints.find_if([&](auto const& breakpoint) { return breakpoint.location == previous_location; });
                if (breakpoint == self->m_breakpoints.end())
                    return;

                breakpoint->location.source_id.clear();
                auto tab = self->m_tab.strong_ref();
                if (!tab)
                    return;

                self->devtools().delegate().set_debugger_breakpoint(tab->description(), breakpoint->location, breakpoint->options, [](ErrorOr<void> result) {
                    if (result.is_error())
                        dbgln_if(DEVTOOLS_DEBUG, "Unable to reapply debugger breakpoint: {}", result.error());
                });
            });
    }
}

void BreakpointListActor::send_breakpoint_error(Message const& message, Error const& error)
{
    JsonObject response;
    response.set("error"sv, "breakpointFailed"sv);
    response.set("message"sv, MUST(String::formatted("{}", error)));
    send_response(message, move(response));
}

}
