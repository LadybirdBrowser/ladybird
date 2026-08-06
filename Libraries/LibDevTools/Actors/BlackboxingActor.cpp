/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/BlackboxingActor.h>
#include <LibDevTools/Actors/WatcherActor.h>

namespace DevTools {

NonnullRefPtr<BlackboxingActor> BlackboxingActor::create(DevToolsServer& devtools, String name, WeakPtr<WatcherActor> watcher)
{
    return adopt_ref(*new BlackboxingActor(devtools, move(name), move(watcher)));
}

BlackboxingActor::BlackboxingActor(DevToolsServer& devtools, String name, WeakPtr<WatcherActor> watcher)
    : Actor(devtools, move(name))
    , m_watcher(move(watcher))
{
}

BlackboxingActor::~BlackboxingActor() = default;

Optional<WebView::DebuggerBlackboxRange> BlackboxingActor::parse_range(JsonObject const& range)
{
    auto start = range.get_object("start"sv);
    auto end = range.get_object("end"sv);
    if (!start.has_value() || !end.has_value())
        return {};

    auto start_line = start->get_integer<u32>("line"sv);
    auto start_column = start->get_integer<u32>("column"sv);
    auto end_line = end->get_integer<u32>("line"sv);
    auto end_column = end->get_integer<u32>("column"sv);
    if (!start_line.has_value() || !start_column.has_value() || !end_line.has_value() || !end_column.has_value())
        return {};

    if (*end_line < *start_line || (*end_line == *start_line && *end_column < *start_column))
        return {};

    return WebView::DebuggerBlackboxRange {
        .start = { *start_line, *start_column },
        .end = { *end_line, *end_column },
    };
}

void BlackboxingActor::handle_message(Message const& message)
{
    if (message.type != "blackbox"sv && message.type != "unblackbox"sv) {
        send_unrecognized_packet_type_error(message);
        return;
    }

    auto url = get_required_parameter<String>(message, "url"sv);
    auto range_values = get_required_parameter<JsonArray>(message, "range"sv);
    if (!url.has_value() || !range_values.has_value())
        return;

    Vector<WebView::DebuggerBlackboxRange> ranges;
    for (auto const& value : range_values->values()) {
        if (!value.is_object()) {
            send_missing_parameter_error(message, "range"sv);
            return;
        }
        auto range = parse_range(value.as_object());
        if (!range.has_value()) {
            send_missing_parameter_error(message, "range"sv);
            return;
        }
        ranges.append(range.release_value());
    }

    if (auto watcher = m_watcher.strong_ref()) {
        auto operation = message.type == "blackbox"sv
            ? WebView::DebuggerBlackboxingOperation::Blackbox
            : WebView::DebuggerBlackboxingOperation::Unblackbox;
        watcher->update_debugger_blackboxing(*url, move(ranges), operation);
    }

    send_response(message, {});
}

}
