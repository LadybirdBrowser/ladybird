/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Application.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteDownloads.h>

namespace Ladybird {

RemoteDownloads& RemoteDownloads::the()
{
    static RemoteDownloads s_the;
    return s_the;
}

static RemoteDownloads::Download download_from(Hub::DownloadState const& state)
{
    return {
        .id = state.id,
        .is_private = state.is_private,
        .url = state.url,
        .destination = LexicalPath { state.destination },
        .status = state.status,
        .downloaded_size = state.downloaded_size,
        .total_size = state.total_size,
        .error = state.error,
        .can_resume = state.can_resume,
        .connection_count = state.connection_count,
        .bytes_per_second = state.bytes_per_second,
        .is_waiting_to_retry = state.is_waiting_to_retry,
        .created_time = state.created_time,
    };
}

void RemoteDownloads::set_downloads(ReadonlySpan<Hub::DownloadState> downloads)
{
    m_downloads.clear();
    m_segments.clear();
    for (auto const& download : downloads) {
        m_downloads.append(download_from(download));
        m_segments.set(download.id, download.segments);
    }
}

void RemoteDownloads::did_add(Hub::DownloadState const& state)
{
    m_downloads.append(download_from(state));
    m_segments.set(state.id, state.segments);
    for (auto& observer : m_observers)
        observer.download_added(m_downloads.last());
}

void RemoteDownloads::did_update(Hub::DownloadState const& state)
{
    for (auto& download : m_downloads) {
        if (download.id != state.id)
            continue;
        download = download_from(state);
        m_segments.set(state.id, state.segments);
        for (auto& observer : m_observers)
            observer.download_updated(download);
        return;
    }
}

void RemoteDownloads::did_remove(u64 id, WebView::IsPrivate is_private)
{
    m_downloads.remove_first_matching([&](auto const& download) { return download.id == id; });
    m_segments.remove(id);
    for (auto& observer : m_observers)
        observer.download_removed(id, is_private);
}

Vector<RemoteDownloads::Download> RemoteDownloads::downloads_of_session(WebView::IsPrivate is_private) const
{
    Vector<Download> downloads;
    for (auto const& download : m_downloads) {
        if (download.is_private == is_private)
            downloads.append(download);
    }
    return downloads;
}

Optional<RemoteDownloads::Download const&> RemoteDownloads::download(u64 id) const
{
    for (auto const& download : m_downloads) {
        if (download.id == id)
            return download;
    }
    return {};
}

bool RemoteDownloads::has_active_downloads() const
{
    return any_of(m_downloads, [](auto const& download) { return WebView::FileDownloader::status_is_active(download.status); });
}

bool RemoteDownloads::has_unresumable_downloads() const
{
    return any_of(m_downloads, [](auto const& download) { return WebView::FileDownloader::status_is_active(download.status) && !download.can_resume; });
}

void RemoteDownloads::cancel_download(u64 id)
{
    HubConnection::the().async_cancel_download(id);
}

void RemoteDownloads::pause_download(u64 id)
{
    HubConnection::the().async_pause_download(id);
}

void RemoteDownloads::resume_download(u64 id)
{
    HubConnection::the().async_resume_download(id);
}

void RemoteDownloads::open_path(ByteString const& path, StringView error_message)
{
    if (auto path_string = String::from_byte_string(path); !path_string.is_error())
        Application::the().open_file(path_string.value());
    else
        Application::the().display_error_dialog(error_message);
}

// NB: The UI opens a download itself, as only it sees the user ask for that. The Hub cannot have the UI open any file.
void RemoteDownloads::open(u64 id)
{
    auto download = this->download(id);
    if (!download.has_value() || download->status != WebView::FileDownloader::DownloadStatus::Completed)
        return;
    open_path(download->destination.string(), "Unable to open downloaded file: path cannot be represented by this frontend"sv);
}

void RemoteDownloads::show_in_folder(u64 id)
{
    auto download = this->download(id);
    if (!download.has_value() || download->status != WebView::FileDownloader::DownloadStatus::Completed)
        return;
    open_path(download->destination.dirname(), "Unable to show downloaded file: path cannot be represented by this frontend"sv);
}

void RemoteDownloads::prune_inactive_downloads(WebView::IsPrivate is_private)
{
    HubConnection::the().async_prune_inactive_downloads(is_private);
}

void RemoteDownloads::pause_active_downloads()
{
    HubConnection::the().async_pause_active_downloads();
}

void RemoteDownloads::cancel_unresumable_downloads()
{
    HubConnection::the().async_cancel_unresumable_downloads();
}

RemoteDownloadsObserver::RemoteDownloadsObserver()
{
    RemoteDownloads::the().m_observers.append(*this);
}

RemoteDownloadsObserver::~RemoteDownloadsObserver()
{
    RemoteDownloads::the().m_observers.remove_first_matching([&](auto& observer) { return &observer == this; });
}

}
