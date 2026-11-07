/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/RemoteDownloads.h>
#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

class DownloadsPage final
    : public WebUIPage
    , public RemoteDownloadsObserver {
public:
    DownloadsPage(u64 web_ui_id, WebView::IsPrivate);

    virtual void received_message(String const& name, JsonValue const& data) override;

private:
    virtual void download_added(RemoteDownloads::Download const&) override;
    virtual void download_updated(RemoteDownloads::Download const&) override;
    virtual void download_removed(u64, WebView::IsPrivate) override;

    void load_downloads();
    void cancel_download(JsonValue const&);
    void pause_download(JsonValue const&);
    void resume_download(JsonValue const&);
    void open_download(JsonValue const&);
    void show_download_in_folder(JsonValue const&);

    WebView::IsPrivate m_is_private { WebView::IsPrivate::No };
};

}
