/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/FileDownloader.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class DownloadsUI final
    : public WebUI
    , public FileDownloaderObserver {
    WEB_UI(DownloadsUI);

private:
    virtual void register_interfaces() override;

    virtual void download_added(FileDownloader::Download const&) override;
    virtual void download_updated(FileDownloader::Download const&) override;
    virtual void download_removed(u64) override;

    void load_downloads();
    void prune_inactive_downloads();
    void cancel_download(JsonValue const&);
    void open_download(JsonValue const&);
    void show_download_in_folder(JsonValue const&);
};

}
