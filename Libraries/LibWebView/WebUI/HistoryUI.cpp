/*
 * Copyright (c) 2026, Jorge Pais <jorge27pais@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/WebUI/HistoryUI.h>

namespace WebView {

static constexpr auto URL_KEY = "url"sv;
static constexpr auto TITLE_KEY = "title"sv;
static constexpr auto FAVICON_KEY = "favicon"sv;
static constexpr auto DATE_VISITED_KEY = "dateVisited"sv;
static constexpr auto TIMES_VISITED_KEY = "timesVisited"sv;

static JsonObject serialize_history_entry(HistoryEntry const& entry)
{
    JsonObject object;

    object.set(URL_KEY, entry.url);
    object.set(DATE_VISITED_KEY, entry.last_visited_time.milliseconds_since_epoch());
    object.set(TIMES_VISITED_KEY, entry.visit_count);
    if (entry.title.has_value())
        object.set(TITLE_KEY, entry.title.value());
    if (entry.favicon_base64_png.has_value())
        object.set(FAVICON_KEY, entry.favicon_base64_png.value());

    return object;
}

static JsonValue serialize_history_list(Vector<HistoryEntry> const& list)
{
    JsonArray items;
    items.ensure_capacity(list.size());

    for (auto const& entry : list) {
        items.must_append(serialize_history_entry(entry));
    }

    return items;
}

void HistoryUI::register_interfaces()
{
    register_interface("loadHistory"sv, [this](auto&&) { load_history(); });
    register_interface("deleteHistoryEntry"sv, [this](auto const& data) { delete_history_entry(data); });
}

void HistoryUI::load_history()
{
    auto history = Application::history_store().list_all_entries();
    async_send_message("loadHistory"sv, serialize_history_list(history));
}

void HistoryUI::delete_history_entry(JsonValue const& data)
{
    if (!data.is_object())
        return;

    auto url = data.as_object().get_string(URL_KEY);
    if (!url.has_value())
        return;

    Application::history_store().remove_entry_by_url(url.value());
}

}
