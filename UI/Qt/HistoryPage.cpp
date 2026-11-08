/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWebView/HistoryStore.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/HistoryPage.h>

#include <algorithm>

namespace Ladybird {

static constexpr size_t DEFAULT_HISTORY_PAGE_SIZE = 50;
static constexpr size_t MAX_HISTORY_PAGE_SIZE = 100;

static Optional<String> site_key_for_entry(WebView::HistoryEntry const& entry)
{
    auto parsed_url = URL::Parser::basic_parse(entry.url);
    if (!parsed_url.has_value())
        return {};

    if (!parsed_url->host().has_value() || parsed_url->host()->is_empty_host())
        return {};

    if (auto registrable_domain = parsed_url->host()->registrable_domain(); registrable_domain.has_value())
        return registrable_domain.release_value();

    return MUST(String::from_utf8(parsed_url->serialized_host()));
}

static JsonObject serialize_history_entry(WebView::HistoryEntry const& entry)
{
    String favicon_base64_png;
    if (entry.favicon_png.has_value()) {
        if (auto encoded = encode_base64(entry.favicon_png->bytes()); !encoded.is_error())
            favicon_base64_png = encoded.release_value();
    }

    JsonObject serialized;
    serialized.set("url"sv, entry.url);
    serialized.set("title"sv, entry.title.value_or(String {}));
    serialized.set("faviconBase64Png"sv, move(favicon_base64_png));
    serialized.set("visitCount"sv, entry.visit_count);
    serialized.set("lastVisitedTime"sv, entry.last_visited_time.milliseconds_since_epoch());
    serialized.set("siteKey"sv, site_key_for_entry(entry).value_or(String {}));
    return serialized;
}

HistoryPage::HistoryPage(u64 web_ui_id, WebView::IsPrivate is_private)
    : WebUIPage(web_ui_id)
    , m_is_private(is_private)
{
}

void HistoryPage::received_message(String const& name, JsonValue const& data)
{
    if (name == "loadHistoryEntries"sv)
        load_history_entries(data);
    else if (name == "removeHistoryEntry"sv)
        remove_history_entry(data);
    else if (name == "forgetHistorySite"sv)
        forget_history_site(data);
}

void HistoryPage::load_history_entries(JsonValue const& data)
{
    if (!data.is_object())
        return;

    auto const& object = data.as_object();

    auto offset = object.get_integer<size_t>("offset"sv).value_or(0);
    auto limit = object.get_integer<size_t>("limit"sv);
    auto request_id = object.get_integer<i64>("requestId"sv).value_or(0);
    auto query = object.get_string("query"sv).value_or(String {});

    if (limit.has_value())
        limit = min(*limit, MAX_HISTORY_PAGE_SIZE);
    else
        limit = DEFAULT_HISTORY_PAGE_SIZE;

    // NB: A private window keeps no history, so its page shows none of the normal windows' history.
    Vector<WebView::HistoryEntry> entries;
    if (m_is_private == WebView::IsPrivate::No)
        entries = Application::history_store().list_entries(query, offset, *limit + 1);
    auto has_more = entries.size() > *limit;
    if (has_more)
        entries.resize(*limit);

    JsonArray serialized_entries;
    serialized_entries.ensure_capacity(entries.size());
    for (auto const& entry : entries)
        serialized_entries.must_append(serialize_history_entry(entry));

    JsonObject result;
    result.set("requestId"sv, request_id);
    result.set("query"sv, query);
    result.set("offset"sv, offset);
    result.set("hasMore"sv, has_more);
    result.set("entries"sv, move(serialized_entries));

    send_message("loadHistoryEntries"_string, move(result));
}

void HistoryPage::remove_history_entry(JsonValue const& data)
{
    if (m_is_private == WebView::IsPrivate::Yes || !data.is_object())
        return;

    auto url = data.as_object().get_string("url"sv);
    if (!url.has_value())
        return;

    auto parsed_url = URL::Parser::basic_parse(*url);
    if (!parsed_url.has_value())
        return;

    // NB: What was typed to reach a bookmarked page stays, so the omnibox keeps offering it.
    auto remove_engagements = Application::bookmark_store().is_bookmarked(*parsed_url) ? WebView::RemoveHistoryEntryEngagements::No : WebView::RemoveHistoryEntryEngagements::Yes;
    Application::history_store().remove_entry_for_url(*parsed_url, remove_engagements);
}

void HistoryPage::forget_history_site(JsonValue const& data)
{
    if (m_is_private == WebView::IsPrivate::Yes || !data.is_object())
        return;

    auto url = data.as_object().get_string("url"sv);
    if (!url.has_value())
        return;

    auto parsed_url = URL::Parser::basic_parse(*url);
    if (!parsed_url.has_value())
        return;

    Application::history_store().remove_entries_for_same_site(*parsed_url);
}

}
