/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibCore/ElapsedTimer.h>
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
    ActiveDownload(NonnullOwnPtr<Core::File> file, LexicalPath temporary_destination)
        : file(move(file))
        , temporary_destination(move(temporary_destination))
    {
    }

    ActiveDownload(NonnullRefPtr<Requests::Request> request, NonnullOwnPtr<Core::File> file, LexicalPath temporary_destination)
        : request(move(request))
        , file(move(file))
        , temporary_destination(move(temporary_destination))
    {
    }

    RefPtr<Requests::Request> request;
    Function<void()> on_cancel;
    OwnPtr<Core::File> file;
    LexicalPath temporary_destination;
    Core::ElapsedTimer progress_update_timer;
    bool stopped { false };
};

FileDownloader::FileDownloader() = default;
FileDownloader::~FileDownloader() = default;

static LexicalPath temporary_destination_for(LexicalPath const& destination, u64 download_id)
{
    return LexicalPath { ByteString::formatted("{}.{}.{}.download", destination.string(), download_id, generate_random_uuid()) };
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
    auto download_id = start_download(url, move(destination));
    auto* active = active_download(download_id);
    if (!active)
        return download_id;

    // FIXME: What other request headers should be set? Perhaps we want to use exactly the same request headers used to
    //        originally fetch the image in WebContent.
    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });

    auto request = Application::request_server_client().start_request("GET"sv, url, *request_headers);
    if (!request) {
        fail_download(download_id, "Unable to start request to download file"_string);
        return download_id;
    }

    auto request_ref = request.release_nonnull();
    attach_request_to_download(download_id, request_ref);

    return download_id;
}

u64 FileDownloader::adopt_download(URL::URL const& url, LexicalPath destination, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ReadonlyBytes initial_data)
{
    auto download_id = start_download(url, move(destination), total_size);
    auto* active = active_download(download_id);
    if (!active)
        return download_id;

    if (!initial_data.is_empty()) {
        append_download_data(download_id, initial_data);
        auto download = this->download(download_id);
        if (!download.has_value() || download->status != DownloadStatus::InProgress)
            return download_id;
    }

    auto request = Application::request_server_client().adopt_request(request_server_client_id, request_server_request_id);
    if (!request) {
        fail_download(download_id, "Unable to adopt request to download file"_string);
        return download_id;
    }

    attach_request_to_download(download_id, request.release_nonnull());
    return download_id;
}

void FileDownloader::attach_request_to_download(u64 download_id, NonnullRefPtr<Requests::Request> request)
{
    auto* active = active_download(download_id);
    if (!active)
        return;

    active->request = request;

    request->set_unbuffered_request_callbacks(
        [this, download_id](NonnullRefPtr<HTTP::HeaderList> response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes>, Optional<u64>) {
            auto* download = mutable_download_or_null(download_id);
            if (!download)
                return;

            auto extracted_length = response_headers->extract_length();
            if (extracted_length.has<u64>())
                download->total_size = extracted_length.get<u64>();

            if (response_code.has_value() && *response_code >= 400) {
                auto error = status_to_error_string({}, response_code, reason_phrase);
                fail_download(download_id, move(error));
                return;
            }

            notify_download_updated(*download);
        },
        [this, download_id](Requests::ResponseData data) {
            append_download_data(download_id, data.bytes());
        },
        [](Core::ImmutableBytes) {
        },
        [this, download_id](u64 total_size, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            auto* download = mutable_download_or_null(download_id);
            if (!download)
                return;

            download->total_size = total_size;

            if (network_error.has_value())
                fail_download(download_id, status_to_error_string(network_error, {}, {}));
            else
                finish_download(download_id);
        });
}

u64 FileDownloader::start_download(URL::URL const& url, LexicalPath destination, Optional<u64> total_size)
{
    auto download_id = m_next_download_id++;

    m_downloads.append(Download {
        .id = download_id,
        .url = url,
        .destination = move(destination),
        .total_size = total_size,
        .error = {},
    });
    auto& download = m_downloads.last();
    notify_download_added(download);

    auto temporary_destination = temporary_destination_for(download.destination, download_id);
    auto file_or_error = Core::File::open(temporary_destination.string(), Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew);
    if (file_or_error.is_error()) {
        fail_download(download_id, MUST(String::formatted("Unable to save downloaded file: {}", file_or_error.error())));
        return download_id;
    }
    auto file = file_or_error.release_value();

    m_active_downloads.set(download_id, make<ActiveDownload>(move(file), temporary_destination));

    return download_id;
}

bool FileDownloader::has_active_downloads() const
{
    for (auto const& download : m_downloads) {
        if (download.status == DownloadStatus::InProgress)
            return true;
    }

    return false;
}

void FileDownloader::append_download_data(u64 id, ReadonlyBytes bytes)
{
    constexpr i64 progress_update_interval_ms = 100;

    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    auto* active = active_download(id);
    if (!active || active->stopped)
        return;

    if (auto result = active->file->write_until_depleted(bytes); result.is_error()) {
        fail_download(id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
        return;
    }

    download->downloaded_size += bytes.size();
    if (active->progress_update_timer.is_valid() && active->progress_update_timer.elapsed_milliseconds() < progress_update_interval_ms)
        return;

    active->progress_update_timer.start();
    notify_download_updated(*download);
}

void FileDownloader::finish_download(u64 id)
{
    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    auto* active = active_download(id);
    if (!active) {
        notify_download_updated(*download);
        return;
    }

    active->file = nullptr;

    if (auto result = Core::System::rename(active->temporary_destination.string(), download->destination.string()); result.is_error()) {
        fail_download(id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
        return;
    }

    download->status = DownloadStatus::Completed;
    if (!download->total_size.has_value())
        download->total_size = download->downloaded_size;
    notify_download_updated(*download);

    Core::deferred_invoke([this, id] {
        m_active_downloads.remove(id);
    });
}

Optional<FileDownloader::Download const&> FileDownloader::download(u64 id) const
{
    for (auto const& download : m_downloads) {
        if (download.id == id)
            return download;
    }
    return {};
}

FileDownloader::Download* FileDownloader::mutable_download_or_null(u64 id)
{
    for (auto& download : m_downloads) {
        if (download.id == id)
            return &download;
    }
    return nullptr;
}

FileDownloader::ActiveDownload* FileDownloader::active_download(u64 id)
{
    if (auto active_download = m_active_downloads.get(id); active_download.has_value())
        return active_download.value();
    return nullptr;
}

void FileDownloader::set_cancel_callback(u64 id, Function<void()> on_cancel)
{
    if (auto* active = active_download(id))
        active->on_cancel = move(on_cancel);
}

void FileDownloader::discard_active_download(u64 id)
{
    auto* active = active_download(id);
    if (!active)
        return;

    active->stopped = true;
    if (active->request)
        active->request->stop();
    if (active->on_cancel)
        active->on_cancel();

    active->file = nullptr;
    (void)Core::System::unlink(active->temporary_destination.string());

    Core::deferred_invoke([this, id] {
        m_active_downloads.remove(id);
    });
}

void FileDownloader::cancel_active_downloads()
{
    Vector<u64> active_download_ids;
    for (auto const& download : m_downloads) {
        if (download.status == DownloadStatus::InProgress)
            active_download_ids.append(download.id);
    }

    for (auto id : active_download_ids)
        cancel_download(id);
}

void FileDownloader::cancel_download(u64 id)
{
    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    download->status = DownloadStatus::Canceled;
    download->error = {};

    discard_active_download(id);

    notify_download_updated(*download);
}

void FileDownloader::fail_download(u64 id, String error)
{
    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    download->status = DownloadStatus::Failed;
    download->error = move(error);

    discard_active_download(id);

    notify_download_updated(*download);
}

Vector<u64> FileDownloader::prune_inactive_downloads()
{
    Vector<u64> removed_download_ids;

    for (size_t i = m_downloads.size(); i > 0; --i) {
        auto const index = i - 1;
        auto const id = m_downloads[index].id;
        if (m_downloads[index].status == DownloadStatus::InProgress)
            continue;

        m_active_downloads.remove(id);
        m_downloads.remove(index);
        removed_download_ids.append(id);
    }

    for (auto id : removed_download_ids)
        notify_download_removed(id);

    return removed_download_ids;
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

void FileDownloader::notify_download_removed(u64 id)
{
    for (auto& observer : m_observers)
        observer.download_removed(id);
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
