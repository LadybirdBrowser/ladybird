/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <Hub/HubClientEndpoint.h>
#include <LibWebView/FileDownloader.h>

namespace Ladybird {

class RemoteDownloadsObserver;

// The Hub's downloads, as the UI shows them.
class RemoteDownloads {
public:
    using Download = WebView::FileDownloader::Download;

    static RemoteDownloads& the();

    void set_downloads(ReadonlySpan<Hub::DownloadState>);
    void did_add(Hub::DownloadState const&);
    void did_update(Hub::DownloadState const&);
    void did_remove(u64 id, WebView::IsPrivate);

    Vector<Download> downloads_of_session(WebView::IsPrivate) const;
    Optional<Download const&> download(u64 id) const;
    bool has_active_downloads() const;
    bool has_unresumable_downloads() const;

    void cancel_download(u64 id);
    void pause_download(u64 id);
    void resume_download(u64 id);
    void show_in_folder(u64 id);
    void prune_inactive_downloads(WebView::IsPrivate);
    void pause_active_downloads();
    void cancel_unresumable_downloads();

private:
    friend class RemoteDownloadsObserver;

    Vector<Download> m_downloads;
    Vector<RemoteDownloadsObserver&> m_observers;
};

class RemoteDownloadsObserver {
public:
    RemoteDownloadsObserver();
    virtual ~RemoteDownloadsObserver();

    virtual void download_added(RemoteDownloads::Download const&) { }
    virtual void download_updated(RemoteDownloads::Download const&) { }
    virtual void download_removed(u64, WebView::IsPrivate) { }
};

}
