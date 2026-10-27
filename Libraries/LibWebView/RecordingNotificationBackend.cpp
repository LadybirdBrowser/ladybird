/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
            if (on_shown)
                on_shown(notification.id);
            return;
        }
    }

    m_shown.append(notification);
    if (on_shown)
        on_shown(notification.id);
}

void RecordingNotificationBackend::close(u64 id)
{
    if (auto index = index_of(id); index.has_value())
        m_shown.remove(*index);
    m_closed.append(id);
    if (on_closed)
        on_closed(id, ByUser::No);
}

void RecordingNotificationBackend::click(u64 id)
{
    if (!index_of(id).has_value())
        return;
    if (on_clicked)
        on_clicked(id);
}

void RecordingNotificationBackend::dismiss(u64 id)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    m_shown.remove(*index);
    m_closed.append(id);
    if (on_closed)
        on_closed(id, ByUser::Yes);
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
