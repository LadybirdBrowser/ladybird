/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/View.h>
#include <LibWebView/ExternalURLHandler.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/PictureInPictureWindow.h>

namespace Hub {

// These stand in for what only the UI can do, as it has the windows and the desktop.

class PictureInPictureWindowInUI final : public WebView::PictureInPictureWindow {
public:
    PictureInPictureWindowInUI(View&, u64 owner_view_id, Gfx::IntSize video_size, Gfx::IntSize initial_size);
    virtual ~PictureInPictureWindowInUI() override;

    static PictureInPictureWindowInUI* find(u64 view_id);

    void did_resize(Gfx::IntSize);
    void did_close(bool video_keeps_playing);

private:
    virtual Gfx::IntSize size() const override { return m_size; }
    virtual String handle() const override { return m_view.handle(); }
    virtual void hide() override;
    virtual void set_video_size(Gfx::IntSize) override;

    View& m_view;
    u64 m_view_id { 0 };
    Gfx::IntSize m_size;
};

class NotificationBackendInUI final : public WebView::NotificationBackend {
public:
    NotificationBackendInUI();
    virtual ~NotificationBackendInUI() override;

    static NotificationBackendInUI* the();

private:
    virtual void show(WebView::PlatformNotification const&) override;
    virtual void close(u64 id) override;
};

class ExternalURLHandlerInUI final : public WebView::ExternalURLHandler {
public:
    ExternalURLHandlerInUI(u64 handler_id, String application_name, bool is_ladybird);
    virtual ~ExternalURLHandlerInUI() override;

    virtual void launch(URL::URL const&, LaunchCallback) const override;

private:
    u64 m_handler_id { 0 };
};

}
