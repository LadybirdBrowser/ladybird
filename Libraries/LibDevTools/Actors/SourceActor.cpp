/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NumericLimits.h>
#include <LibDevTools/Actors/BlackboxingActor.h>
#include <LibDevTools/Actors/SourceActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WatcherActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

static bool position_is_within_query(WebView::DebuggerSourcePosition const& position, JsonObject const& query)
{
    auto start = query.get_object("start"sv).value_or({});
    auto end = query.get_object("end"sv).value_or({});
    auto start_line = start.get_integer<u32>("line"sv).value_or(0);
    auto start_column = start.get_integer<u32>("column"sv).value_or(0);
    auto end_line = end.get_integer<u32>("line"sv).value_or(NumericLimits<u32>::max());
    auto end_column = end.get_integer<u32>("column"sv).value_or(NumericLimits<u32>::max());

    if (position.line < start_line || (position.line == start_line && position.column < start_column))
        return false;
    if (position.line > end_line || (position.line == end_line && position.column >= end_column))
        return false;
    return true;
}

NonnullRefPtr<SourceActor> SourceActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WatcherActor> watcher, Web::HTML::ScriptRegistry::Description source)
{
    return adopt_ref(*new SourceActor(devtools, move(name), move(tab), move(watcher), move(source)));
}

SourceActor::SourceActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WatcherActor> watcher, Web::HTML::ScriptRegistry::Description source)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_watcher(move(watcher))
    , m_source(move(source))
{
}

SourceActor::~SourceActor() = default;

JsonObject SourceActor::serialize_source() const
{
    auto source_url = this->source_url();

    JsonObject source;
    source.set("actor"sv, name());
    source.set("extensionName"sv, JsonValue {});
    source.set("url"sv, source_url);
    auto is_blackboxed = false;
    if (auto watcher = m_watcher.strong_ref())
        is_blackboxed = watcher->is_source_fully_blackboxed(source_url);
    source.set("isBlackBoxed"sv, is_blackboxed);
    source.set("sourceMapBaseURL"sv, source_url);
    source.set("sourceMapURL"sv, JsonValue {});
    source.set("introductionType"sv, m_source.introduction_type.to_utf8());
    source.set("isInlineSource"sv, m_source.is_inline_source);
    source.set("sourceStartLine"sv, m_source.source_start_line);
    source.set("sourceStartColumn"sv, m_source.source_start_column);
    source.set("sourceLength"sv, static_cast<i64>(m_source.source_length));
    if (auto tab = m_tab.strong_ref()) {
        source.set("browsingContextID"sv, tab->description().id);
        source.set("innerWindowId"sv, tab->inner_window_id());
        source.set("resourceId"sv, MUST(String::formatted("source-{}-{}-{}", tab->inner_window_id(), m_source.id.document_id.value(), m_source.id.script_id)));
    }
    return source;
}

String SourceActor::source_url() const
{
    return m_source.url.has_value() ? m_source.url->serialize() : m_source.display_url.to_utf8();
}

void SourceActor::handle_message(Message const& message)
{
    if (message.type == "source"sv) {
        auto tab = m_tab.strong_ref();
        if (!tab) {
            JsonObject response;
            response.set("source"sv, ""sv);
            response.set("contentType"sv, m_source.content_type.to_utf8());
            send_response(message, move(response));
            return;
        }

        devtools().delegate().retrieve_source(tab->description(), m_source.id,
            async_handler<SourceActor>(message, [](auto&, auto source_content, auto& response) {
                // Firefox's longstring protocol accepts a primitive string and wraps it in a SimpleStringFront on the
                // client side.
                response.set("source"sv, source_content.text.to_utf8());
                response.set("contentType"sv, source_content.content_type.to_utf8());
            }));
        return;
    }

    if (message.type == "getBreakableLines"sv) {
        auto tab = m_tab.strong_ref();
        if (!tab) {
            JsonObject response;
            response.set("lines"sv, JsonArray {});
            send_response(message, move(response));
            return;
        }

        devtools().delegate().retrieve_debugger_source_positions(tab->description(), m_source.id,
            async_handler<SourceActor>(message, [](auto&, auto positions, auto& response) {
                JsonArray lines;
                Optional<u32> previous_line;
                for (auto const& position : positions) {
                    if (previous_line == position.line)
                        continue;
                    lines.must_append(position.line);
                    previous_line = position.line;
                }
                response.set("lines"sv, move(lines));
            }));
        return;
    }

    if (message.type == "getBreakpointPositionsCompressed"sv) {
        auto tab = m_tab.strong_ref();
        if (!tab) {
            JsonObject response;
            response.set("positions"sv, JsonObject {});
            send_response(message, move(response));
            return;
        }

        auto query = message.data.get_object("query"sv).value_or({});
        devtools().delegate().retrieve_debugger_source_positions(tab->description(), m_source.id,
            async_handler<SourceActor>(message, [query = move(query)](auto&, auto positions, auto& response) {
                JsonObject compressed_positions;
                for (auto const& position : positions) {
                    if (!position_is_within_query(position, query))
                        continue;
                    auto line = MUST(String::formatted("{}", position.line));
                    auto columns = compressed_positions.get_array(line).value_or({});
                    columns.must_append(position.column);
                    compressed_positions.set(move(line), move(columns));
                }
                response.set("positions"sv, move(compressed_positions));
            }));
        return;
    }

    if (message.type == "setPausePoints"sv) {
        // FIXME: Apply source pause points when choosing stepping locations.
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "blackbox"sv || message.type == "unblackbox"sv) {
        auto source_url = this->source_url();
        Vector<WebView::DebuggerBlackboxRange> ranges;
        if (auto range_value = message.data.get("range"sv); range_value.has_value() && !range_value->is_null()) {
            if (!range_value->is_object()) {
                send_missing_parameter_error(message, "range"sv);
                return;
            }
            auto range = BlackboxingActor::parse_range(range_value->as_object());
            if (!range.has_value()) {
                send_missing_parameter_error(message, "range"sv);
                return;
            }
            ranges.append(range.release_value());
        }

        auto operation = message.type == "blackbox"sv
            ? WebView::DebuggerBlackboxingOperation::Blackbox
            : WebView::DebuggerBlackboxingOperation::Unblackbox;
        if (auto watcher = m_watcher.strong_ref())
            watcher->update_debugger_blackboxing(source_url, move(ranges), operation);

        JsonObject response;
        if (message.type == "blackbox"sv) {
            auto paused_in_source = false;
            if (auto watcher = m_watcher.strong_ref())
                paused_in_source = watcher->is_paused_in_source(m_source.id);
            response.set("pausedInSource"sv, paused_in_source);
        }
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
