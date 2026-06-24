/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibIPC/File.h>
#include <LibRequests/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API FileDownloader {
public:
    FileDownloader();
    ~FileDownloader();

    void download_file(URL::URL const&, LexicalPath);

    Optional<IPC::File> begin_streamed_download(u64 page_id, u64 download_id, URL::URL const&, String const& suggested_name);
    void streamed_download_finished(u64 page_id, u64 download_id);
    void streamed_download_failed(u64 page_id, u64 download_id, String const& error);

    struct StreamedDownloadKey {
        u64 page_id { 0 };
        u64 download_id { 0 };
        bool operator==(StreamedDownloadKey const&) const = default;
    };

private:
    HashMap<u64, NonnullRefPtr<Requests::Request>> m_requests;
    HashMap<StreamedDownloadKey, LexicalPath> m_streamed_downloads;
};

}

namespace AK {

template<>
struct Traits<WebView::FileDownloader::StreamedDownloadKey> : public DefaultTraits<WebView::FileDownloader::StreamedDownloadKey> {
    static unsigned hash(WebView::FileDownloader::StreamedDownloadKey const& key)
    {
        return pair_int_hash(static_cast<u32>(key.page_id), static_cast<u32>(key.download_id));
    }
};

}
