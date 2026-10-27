/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/RecordingNotificationBackend.h>

namespace WebView {

void RecordingNotificationBackend::show(PlatformNotification const& notification)
{
    if (exchange(fail_next_show, false)) {
        if (on_failed)
            on_failed(notification.id);
        return;
    }

    // A replacement takes the replaced notification's place, as a platform that replaces in place would show it.
    if (notification.replaces.has_value()) {
        if (auto index = index_of(*notification.replaces); index.has_value()) {
            m_shown[*index] = notification;
            m_page_of.set(notification.id, notification.page_id);
            if (on_shown)
                on_shown(notification.id);
            return;
        }
    }

    m_shown.append(notification);
    m_page_of.set(notification.id, notification.page_id);
    if (on_shown)
        on_shown(notification.id);
}

void RecordingNotificationBackend::close(u64 id)
{
    if (auto index = index_of(id); index.has_value())
        m_shown.remove(*index);
    m_closed.append({ id, m_page_of.get(id).value_or({}) });
    if (on_closed)
        on_closed(id, ByUser::No);
}

void RecordingNotificationBackend::click(u64 id)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    // A click takes the notification down, as it does on every platform.
    m_shown.remove(*index);
    m_closed.append({ id, m_page_of.get(id).value_or({}) });
    if (on_clicked)
        on_clicked(id);
    if (on_closed)
        on_closed(id, ByUser::Yes);
}

void RecordingNotificationBackend::dismiss(u64 id)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    m_shown.remove(*index);
    m_closed.append({ id, m_page_of.get(id).value_or({}) });
    if (on_closed)
        on_closed(id, ByUser::Yes);
}

void RecordingNotificationBackend::record_focus(Web::PageId page_id)
{
    m_focused.append(page_id);
}

void RecordingNotificationBackend::record_open(URL::URL const& url, Web::PageId originating_page)
{
    m_opened.append({ url, originating_page });
}

void RecordingNotificationBackend::forget_page(Web::PageId page_id)
{
    m_shown.remove_all_matching([&](auto const& notification) { return notification.page_id == page_id; });
    m_closed.remove_all_matching([&](auto const& entry) { return entry.page_id == page_id; });
    m_focused.remove_all_matching([&](auto const& focused) { return focused == page_id; });
    m_opened.remove_all_matching([&](auto const& entry) { return entry.page_id == page_id; });
}

String RecordingNotificationBackend::state_json(Web::PageId page_id) const
{
    JsonArray shown;
    for (auto const& notification : m_shown) {
        if (notification.page_id != page_id)
            continue;
        JsonObject entry;
        entry.set("id"sv, notification.id);
        entry.set("title"sv, notification.data.title.to_utf8());
        entry.set("body"sv, notification.data.body.to_utf8());
        entry.set("tag"sv, notification.data.tag.to_utf8());
        entry.set("attribution"sv, notification.attribution);
        entry.set("silent"sv, notification.data.silent.has_value() ? JsonValue(*notification.data.silent) : JsonValue());
        entry.set("requireInteraction"sv, notification.data.require_interaction);
        entry.set("renotify"sv, notification.data.renotify);
        entry.set("hasIcon"sv, !notification.icon.is_null());
        if (notification.icon) {
            entry.set("iconWidth"sv, notification.icon->width());
            entry.set("iconHeight"sv, notification.icon->height());
        }
        entry.set("replaces"sv, notification.replaces.has_value() ? JsonValue(*notification.replaces) : JsonValue());
        shown.must_append(move(entry));
    }

    JsonArray closed;
    for (auto const& entry : m_closed) {
        if (entry.page_id == page_id)
            closed.must_append(entry.id);
    }

    JsonArray focused;
    for (auto focused_page_id : m_focused) {
        if (focused_page_id == page_id)
            focused.must_append(focused_page_id.value());
    }

    JsonArray opened;
    for (auto const& entry : m_opened) {
        if (entry.page_id == page_id)
            opened.must_append(entry.url.serialize());
    }

    JsonObject state;
    state.set("shown"sv, move(shown));
    state.set("closed"sv, move(closed));
    state.set("focused"sv, move(focused));
    state.set("opened"sv, move(opened));
    return state.serialized();
}

Optional<size_t> RecordingNotificationBackend::index_of(u64 id) const
{
    for (size_t index = 0; index < m_shown.size(); ++index) {
        if (m_shown[index].id == id)
            return index;
    }
    return {};
}

}
