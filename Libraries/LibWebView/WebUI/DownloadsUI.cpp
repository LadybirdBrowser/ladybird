/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebUI/DownloadsUI.h>

namespace WebView {

static StringView status_to_string(FileDownloader::DownloadStatus status)
{
    switch (status) {
    case FileDownloader::DownloadStatus::InProgress:
        return "in-progress"sv;
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
    return serialized;
}

void DownloadsUI::register_interfaces()
{
    register_interface("loadDownloads"sv, [this](auto const&) {
        load_downloads();
    });
    register_interface("pruneInactiveDownloads"sv, [this](auto const&) {
        prune_inactive_downloads();
    });
    register_interface("cancelDownload"sv, [this](auto const& data) {
        cancel_download(data);
    });
    register_interface("openDownload"sv, [this](auto const& data) {
        open_download(data);
    });
    register_interface("showDownloadInFolder"sv, [this](auto const& data) {
        show_download_in_folder(data);
    });
}

void DownloadsUI::download_added(FileDownloader::Download const& download)
{
    async_send_message("downloadAdded"sv, serialize_download(download));
}

void DownloadsUI::download_updated(FileDownloader::Download const& download)
{
    async_send_message("downloadUpdated"sv, serialize_download(download));
}

void DownloadsUI::download_removed(u64 id)
{
    async_send_message("downloadRemoved"sv, JsonValue { id });
}

void DownloadsUI::load_downloads()
{
    auto downloads = Application::the().file_downloader().downloads();

    JsonArray serialized_downloads;
    serialized_downloads.ensure_capacity(downloads.size());
    for (auto const& download : downloads)
        serialized_downloads.must_append(serialize_download(download));

    async_send_message("loadDownloads"sv, move(serialized_downloads));
}

void DownloadsUI::prune_inactive_downloads()
{
    (void)Application::the().file_downloader().prune_inactive_downloads();
}

static Optional<FileDownloader::Download const&> download_from_message(JsonValue const& data)
{
    if (!data.is_object())
        return {};

    auto id = data.as_object().get_integer<u64>("id"sv);
    if (!id.has_value())
        return {};

    return Application::the().file_downloader().download(*id);
}

void DownloadsUI::cancel_download(JsonValue const& data)
{
    auto download = download_from_message(data);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::InProgress)
        return;

    Application::the().file_downloader().cancel_download(download->id);
}

void DownloadsUI::open_download(JsonValue const& data)
{
    auto download = download_from_message(data);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::Completed)
        return;

    Application::the().open_download(*download);
}

void DownloadsUI::show_download_in_folder(JsonValue const& data)
{
    auto download = download_from_message(data);
    if (!download.has_value())
        return;

    if (download->status != FileDownloader::DownloadStatus::Completed)
        return;

    Application::the().show_download_in_folder(*download);
}

}
