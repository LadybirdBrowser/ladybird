/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>

namespace WebView {

struct FileDownloader::ActiveDownload {
    ActiveDownload(NonnullRefPtr<Requests::Request> request, NonnullOwnPtr<Core::File> file, LexicalPath temporary_destination)
        : request(move(request))
        , file(move(file))
        , temporary_destination(move(temporary_destination))
    {
    }

    NonnullRefPtr<Requests::Request> request;
    OwnPtr<Core::File> file;
    LexicalPath temporary_destination;
    bool failed { false };
};

FileDownloader::FileDownloader() = default;
FileDownloader::~FileDownloader() = default;

static LexicalPath temporary_destination_for(LexicalPath const& destination)
{
    return LexicalPath { ByteString::formatted("{}.download", destination.string()) };
}

static String status_to_error_string(Optional<Requests::NetworkError> const& network_error, Optional<u32> response_code, Optional<String> const& reason_phrase)
{
    if (network_error.has_value())
        return MUST(String::formatted("Unable to download file: {}", Requests::network_error_to_string(*network_error)));

    VERIFY(response_code.has_value());
    if (reason_phrase.has_value())
        return MUST(String::formatted("Received error response code {} while downloading file: {}", *response_code, *reason_phrase));
    return MUST(String::formatted("Received error response code {} while downloading file", *response_code));
}

u64 FileDownloader::download_file(URL::URL const& url, LexicalPath destination)
{
    auto download_id = m_next_download_id++;

    m_downloads.append(Download {
        .id = download_id,
        .url = url,
        .destination = move(destination),
    });
    auto& download = m_downloads.last();
    notify_download_added(download);

    auto temporary_destination = temporary_destination_for(download.destination);
    auto file_or_error = Core::File::open(temporary_destination.string(), Core::File::OpenMode::Write);
    if (file_or_error.is_error()) {
        fail_download(download_id, MUST(String::formatted("Unable to save downloaded file: {}", file_or_error.error())));
        return download_id;
    }
    auto file = file_or_error.release_value();

    // FIXME: What other request headers should be set? Perhaps we want to use exactly the same request headers used to
    //        originally fetch the image in WebContent.
    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });

    auto request = Application::request_server_client().start_request("GET"sv, url, *request_headers);
    if (!request) {
        file->close();
        (void)Core::System::unlink(temporary_destination.string());
        fail_download(download_id, "Unable to start request to download file"_string);
        return download_id;
    }

    auto request_ref = request.release_nonnull();
    auto active_download_state = make<ActiveDownload>(request_ref, move(file), temporary_destination);
    m_active_downloads.set(download_id, move(active_download_state));

    request_ref->set_unbuffered_request_callbacks(
        [this, download_id](NonnullRefPtr<HTTP::HeaderList> response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes>, Optional<u64>) {
            auto& download = mutable_download(download_id);

            auto extracted_length = response_headers->extract_length();
            if (extracted_length.has<u64>())
                download.total_size = extracted_length.get<u64>();

            if (response_code.has_value() && *response_code >= 400) {
                auto error = status_to_error_string({}, response_code, reason_phrase);
                fail_download(download_id, move(error));
                return;
            }

            notify_download_updated(download);
        },
        [this, download_id](Requests::ResponseData data) {
            auto* active = active_download(download_id);
            if (!active || active->failed)
                return;

            auto& download = mutable_download(download_id);
            auto bytes = data.bytes();

            if (auto result = active->file->write_until_depleted(bytes); result.is_error()) {
                fail_download(download_id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
                return;
            }

            download.downloaded_size += bytes.size();
            notify_download_updated(download);
        },
        [](Core::ImmutableBytes) {
        },
        [this, download_id](u64 total_size, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            auto& download = mutable_download(download_id);
            download.total_size = total_size;

            auto* active = active_download(download_id);
            if (!active) {
                notify_download_updated(download);
                return;
            }

            active->file = nullptr;

            if (network_error.has_value())
                fail_download(download_id, status_to_error_string(network_error, {}, {}));

            if (!active->failed) {
                if (auto result = Core::System::rename(active->temporary_destination.string(), download.destination.string()); result.is_error()) {
                    fail_download(download_id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
                } else {
                    download.status = DownloadStatus::Completed;
                    download.downloaded_size = total_size;
                    notify_download_updated(download);
                }
            }

            Core::deferred_invoke([this, download_id] {
                m_active_downloads.remove(download_id);
            });
        });

    return download_id;
}

Optional<FileDownloader::Download const&> FileDownloader::download(u64 id) const
{
    for (auto const& download : m_downloads) {
        if (download.id == id)
            return download;
    }
    return {};
}

FileDownloader::Download& FileDownloader::mutable_download(u64 id)
{
    for (auto& download : m_downloads) {
        if (download.id == id)
            return download;
    }
    VERIFY_NOT_REACHED();
}

FileDownloader::ActiveDownload* FileDownloader::active_download(u64 id)
{
    if (auto active_download = m_active_downloads.get(id); active_download.has_value())
        return active_download.value();
    return nullptr;
}

void FileDownloader::fail_download(u64 id, String error)
{
    auto& download = mutable_download(id);
    if (download.status != DownloadStatus::InProgress)
        return;

    download.status = DownloadStatus::Failed;
    download.error = move(error);

    if (auto* active = active_download(id)) {
        active->failed = true;
        active->file = nullptr;
        (void)Core::System::unlink(active->temporary_destination.string());
    }

    notify_download_updated(download);
}

void FileDownloader::notify_download_added(Download const& download)
{
    for (auto& observer : m_observers)
        observer.download_added(download);
}

void FileDownloader::notify_download_updated(Download const& download)
{
    for (auto& observer : m_observers)
        observer.download_updated(download);
}

void FileDownloader::add_observer(Badge<FileDownloaderObserver>, FileDownloaderObserver& observer)
{
    Application::the().file_downloader().m_observers.append(observer);
}

void FileDownloader::remove_observer(Badge<FileDownloaderObserver>, FileDownloaderObserver& observer)
{
    auto was_removed = Application::the().file_downloader().m_observers.remove_first_matching([&](auto const& candidate) {
        return &candidate == &observer;
    });
    VERIFY(was_removed);
}

FileDownloaderObserver::FileDownloaderObserver()
{
    FileDownloader::add_observer({}, *this);
}

FileDownloaderObserver::~FileDownloaderObserver()
{
    FileDownloader::remove_observer({}, *this);
}

}
