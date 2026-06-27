/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

namespace Core {

class File;

}

namespace WebView {

class FileDownloaderObserver;

class WEBVIEW_API FileDownloader {
public:
    enum class DownloadStatus : u8 {
        InProgress,
        Completed,
        Failed,
    };

    struct Download {
        u64 id { 0 };
        URL::URL url;
        LexicalPath destination;
        DownloadStatus status { DownloadStatus::InProgress };
        u64 downloaded_size { 0 };
        Optional<u64> total_size;
        Optional<String> error;
    };

    FileDownloader();
    ~FileDownloader();

    u64 download_file(URL::URL const&, LexicalPath);

    ReadonlySpan<Download> downloads() const { return m_downloads.span(); }
    Optional<Download const&> download(u64 id) const;

    static void add_observer(Badge<FileDownloaderObserver>, FileDownloaderObserver&);
    static void remove_observer(Badge<FileDownloaderObserver>, FileDownloaderObserver&);

private:
    struct ActiveDownload;

    Download& mutable_download(u64 id);
    ActiveDownload* active_download(u64 id);

    void fail_download(u64 id, String);
    void notify_download_added(Download const&);
    void notify_download_updated(Download const&);

    Vector<Download> m_downloads;
    HashMap<u64, NonnullOwnPtr<ActiveDownload>> m_active_downloads;
    Vector<FileDownloaderObserver&> m_observers;
    u64 m_next_download_id { 0 };
};

class WEBVIEW_API FileDownloaderObserver {
public:
    FileDownloaderObserver();
    virtual ~FileDownloaderObserver();

    virtual void download_added(FileDownloader::Download const&) { }
    virtual void download_updated(FileDownloader::Download const&) { }
};

}
