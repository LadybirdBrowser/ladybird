/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>

namespace Hub {

static HashMap<u64, PictureInPictureWindowInUI*> s_picture_in_picture_windows;

PictureInPictureWindowInUI::PictureInPictureWindowInUI(View& view, u64 owner_view_id, Gfx::IntSize video_size, Gfx::IntSize initial_size)
    : m_view(view)
    , m_view_id(view.view_id())
    , m_size(initial_size)
{
    s_picture_in_picture_windows.set(m_view_id, this);
    report_page_close_of(view);
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_open(m_view_id, owner_view_id, video_size);
}

PictureInPictureWindowInUI::~PictureInPictureWindowInUI()
{
    s_picture_in_picture_windows.remove(m_view_id);
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_close(m_view_id);
}

PictureInPictureWindowInUI* PictureInPictureWindowInUI::find(u64 view_id)
{
    return s_picture_in_picture_windows.get(view_id).value_or(nullptr);
}

void PictureInPictureWindowInUI::did_resize(Gfx::IntSize size)
{
    m_size = size;
    if (on_resize)
        on_resize(size);
}

void PictureInPictureWindowInUI::did_close(bool video_keeps_playing)
{
    if (on_close)
        on_close(video_keeps_playing ? VideoKeepsPlaying::Yes : VideoKeepsPlaying::No);
}

void PictureInPictureWindowInUI::hide()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_hide(m_view_id);
}

void PictureInPictureWindowInUI::set_video_size(Gfx::IntSize video_size)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_set_video_size(m_view_id, video_size);
}

static NotificationBackendInUI* s_notification_backend;

NotificationBackendInUI::NotificationBackendInUI()
{
    s_notification_backend = this;
}

NotificationBackendInUI::~NotificationBackendInUI()
{
    s_notification_backend = nullptr;
}

NotificationBackendInUI* NotificationBackendInUI::the()
{
    return s_notification_backend;
}

void NotificationBackendInUI::show(WebView::PlatformNotification const& notification)
{
    Optional<Gfx::ShareableBitmap> icon;
    if (notification.icon)
        icon = notification.icon->to_shareable_bitmap();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_notification_show(notification.id, notification.page_id.value(), notification.replaces, notification.data, notification.attribution, move(icon));
}

void NotificationBackendInUI::close(u64 id)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_notification_close(id);
}

ExternalURLHandlerInUI::ExternalURLHandlerInUI(u64 handler_id, String application_name, bool is_ladybird)
    : ExternalURLHandler(move(application_name), is_ladybird)
    , m_handler_id(handler_id)
{
}

ExternalURLHandlerInUI::~ExternalURLHandlerInUI()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_external_url_release_handler(m_handler_id);
}

void ExternalURLHandlerInUI::launch(URL::URL const& url, LaunchCallback callback) const
{
    Application::the().launch_external_url(m_handler_id, url, move(callback));
}

}
