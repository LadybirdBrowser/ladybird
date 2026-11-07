/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <UI/Qt/DownloadsPage.h>

namespace Ladybird {

using WebView::FileDownloader;

static StringView status_to_string(FileDownloader::DownloadStatus status)
{
    switch (status) {
    case FileDownloader::DownloadStatus::InProgress:
        return "in-progress"sv;
    case FileDownloader::DownloadStatus::Paused:
        return "paused"sv;
    case FileDownloader::DownloadStatus::Completed:
        return "completed"sv;
    case FileDownloader::DownloadStatus::Canceled:
        return "canceled"sv;
    case FileDownloader::DownloadStatus::Failed:
        return "failed"sv;
    }
    VERIFY_NOT_REACHED();
}

static String path_string_for_display(StringView path)
{
    return String::from_utf8_with_replacement_character(path, String::WithBOMHandling::No);
}

static JsonObject serialize_download(FileDownloader::Download const& download)
{
    JsonObject serialized;
    serialized.set("id"sv, download.id);
    serialized.set("url"sv, download.url.serialize());
    serialized.set("fileName"sv, path_string_for_display(download.destination.basename()));
    serialized.set("destination"sv, path_string_for_display(download.destination.string().view()));
    serialized.set("status"sv, status_to_string(download.status));
    serialized.set("downloadedSize"sv, download.downloaded_size);
    serialized.set("totalSize"sv, download.total_size.has_value() ? JsonValue { *download.total_size } : JsonValue {});
    serialized.set("error"sv, download.error.value_or(String {}));
    serialized.set("canResume"sv, download.can_resume);
    serialized.set("connectionCount"sv, download.connection_count);
    serialized.set("isWaitingToRetry"sv, download.is_waiting_to_retry);
    serialized.set("bytesPerSecond"sv, download.bytes_per_second.has_value() ? JsonValue { *download.bytes_per_second } : JsonValue {});

    auto time_remaining = download.estimated_time_remaining();
    serialized.set("secondsRemaining"sv, time_remaining.has_value() ? JsonValue { time_remaining->to_seconds() } : JsonValue {});

    JsonArray segments;
    for (auto const& segment : RemoteDownloads::the().segment_progress(download.id)) {
        JsonObject serialized_segment;
        serialized_segment.set("start"sv, segment.start_offset);
        serialized_segment.set("end"sv, segment.end_offset);
        serialized_segment.set("next"sv, segment.next_offset);

        segments.must_append(move(serialized_segment));
    }
    serialized.set("segments"sv, move(segments));

    return serialized;
}

// The page lists and acts on the downloads of its own browsing session only, so those of a private session stay out of
// the pages of every other session.
static bool is_download_of_session(FileDownloader::Download const& download, WebView::IsPrivate is_private)
{
    return download.is_private == is_private;
}

DownloadsPage::DownloadsPage(u64 web_ui_id, WebView::IsPrivate is_private)
    : WebUIPage(web_ui_id)
    , m_is_private(is_private)
{
}

void DownloadsPage::received_message(String const& name, JsonValue const& data)
{
    if (name == "loadDownloads"sv)
        load_downloads();
    else if (name == "pruneInactiveDownloads"sv)
        RemoteDownloads::the().prune_inactive_downloads(m_is_private);
    else if (name == "cancelDownload"sv)
        cancel_download(data);
    else if (name == "pauseDownload"sv)
        pause_download(data);
    else if (name == "resumeDownload"sv)
        resume_download(data);
    else if (name == "openDownload"sv)
        open_download(data);
    else if (name == "showDownloadInFolder"sv)
        show_download_in_folder(data);
}

void DownloadsPage::download_added(FileDownloader::Download const& download)
{
    if (is_download_of_session(download, m_is_private))
        send_message("downloadAdded"_string, serialize_download(download));
}

void DownloadsPage::download_updated(FileDownloader::Download const& download)
{
    if (is_download_of_session(download, m_is_private))
        send_message("downloadUpdated"_string, serialize_download(download));
}

void DownloadsPage::download_removed(u64 id, WebView::IsPrivate is_private)
{
    if (is_private == m_is_private)
        send_message("downloadRemoved"_string, JsonValue { id });
}

void DownloadsPage::load_downloads()
{
    auto downloads = RemoteDownloads::the().downloads_of_session(m_is_private);

    JsonArray serialized_downloads;
    serialized_downloads.ensure_capacity(downloads.size());
    for (auto const& download : downloads)
        serialized_downloads.must_append(serialize_download(download));

    send_message("loadDownloads"_string, move(serialized_downloads));
}

static Optional<FileDownloader::Download const&> download_from_message(JsonValue const& data, WebView::IsPrivate is_private)
{
    if (!data.is_object())
        return {};

    auto id = data.as_object().get_integer<u64>("id"sv);
    if (!id.has_value())
        return {};

    auto download = RemoteDownloads::the().download(*id);
    if (!download.has_value() || !is_download_of_session(*download, is_private))
        return {};
    return download;
}

void DownloadsPage::cancel_download(JsonValue const& data)
{
    auto download = download_from_message(data, m_is_private);
    if (!download.has_value())
        return;

    if (!FileDownloader::status_is_active(download->status))
        return;

    RemoteDownloads::the().cancel_download(download->id);
}

void DownloadsPage::pause_download(JsonValue const& data)
{
    auto download = download_from_message(data, m_is_private);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::InProgress || !download->can_resume)
        return;

    RemoteDownloads::the().pause_download(download->id);
}

void DownloadsPage::resume_download(JsonValue const& data)
{
    auto download = download_from_message(data, m_is_private);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::Paused)
        return;

    RemoteDownloads::the().resume_download(download->id);
}

// Opening a download can run it, so only the user's own click in the page opens one or shows it in its folder.
void DownloadsPage::open_download(JsonValue const& data)
{
    auto download = download_from_message(data, m_is_private);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::Completed)
        return;

    if (!consume_user_activation())
        return;
    RemoteDownloads::the().open(download->id);
}

void DownloadsPage::show_download_in_folder(JsonValue const& data)
{
    auto download = download_from_message(data, m_is_private);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::Completed)
        return;

    if (!consume_user_activation())
        return;
    RemoteDownloads::the().show_in_folder(download->id);
}

}
