/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibHTTP/HeaderList.h>
#include <LibIPC/File.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>

namespace WebView {

FileDownloader::FileDownloader() = default;
FileDownloader::~FileDownloader() = default;

static ErrorOr<void> save_file(LexicalPath const& destination, ReadonlyBytes data)
{
    auto file = TRY(Core::File::open(destination.string(), Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(data));
    return {};
}

void FileDownloader::download_file(URL::URL const& url, LexicalPath destination)
{
    static u64 next_request_id = 0;

    // FIXME: What other request headers should be set? Perhaps we want to use exactly the same request headers used to
    //        originally fetch the image in WebContent.
    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });

    auto request = Application::request_server_client().start_request("GET"sv, url, *request_headers);
    if (!request) {
        Application::the().display_error_dialog("Unable to start request to download file"sv);
        return;
    }

    auto request_id = next_request_id++;

    request->set_buffered_request_finished_callback(
        [this, request_id, destination = move(destination)](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const&, Optional<u32> response_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes>, Optional<u64>, Core::ImmutableBytes payload) {
            Core::deferred_invoke([this, request_id]() { m_requests.remove(request_id); });

            if (network_error.has_value()) {
                auto error = MUST(String::formatted("Unable to download file: {}", Requests::network_error_to_string(*network_error)));
                Application::the().display_error_dialog(error);
                return;
            }
            if (response_code.has_value() && *response_code >= 400) {
                auto error = reason_phrase.has_value()
                    ? MUST(String::formatted("Received error response code {} while downloading file: {}", *response_code, reason_phrase))
                    : MUST(String::formatted("Received error response code {} while downloading file", *response_code));
                Application::the().display_error_dialog(error);
                return;
            }

            if (auto result = save_file(destination, payload.bytes()); result.is_error()) {
                auto error = MUST(String::formatted("Unable to save downloaded file file: {}", result.error()));
                Application::the().display_error_dialog(error);
            }

            // FIXME: Add a UI element (i.e. a download manager) to indicate download completion.
        });

    m_requests.set(request_id, request.release_nonnull());
}

Optional<IPC::File> FileDownloader::begin_streamed_download(u64 page_id, u64 download_id, URL::URL const& url, String const& suggested_name)
{
    auto name = suggested_name.is_empty() ? url.basename() : suggested_name.to_byte_string();

    auto download_path = Application::the().path_for_downloaded_file(name);
    if (download_path.is_error())
        return {};

    // Stream into a temporary ".part" file — so a download that fails or is interrupted neither destroys an existing
    // file at the destination, nor leaves a truncated one there; on success, the file is renamed into place.
    auto partial_path = MUST(String::formatted("{}.part", download_path.value().string()));
    auto file = Core::File::open(partial_path, Core::File::OpenMode::Write);
    if (file.is_error()) {
        Application::the().display_error_dialog(MUST(String::formatted("Unable to open download destination: {}", file.error())));
        return {};
    }

    m_streamed_downloads.set({ page_id, download_id }, download_path.release_value());
    return IPC::File::adopt_file(file.release_value());
}

void FileDownloader::streamed_download_finished(u64 page_id, u64 download_id)
{
    auto download = m_streamed_downloads.take({ page_id, download_id });
    if (!download.has_value())
        return;

    auto partial_path = MUST(String::formatted("{}.part", download->string()));
    if (auto result = Core::System::rename(partial_path, download->string()); result.is_error()) {
        (void)Core::System::unlink(partial_path);
        Application::the().display_error_dialog(MUST(String::formatted("Unable to save download {}: {}", download->basename(), result.error())));
    }
    // FIXME: Add download-manager UI, to show download completion to users.
}

void FileDownloader::streamed_download_failed(u64 page_id, u64 download_id, String const& error)
{
    auto download = m_streamed_downloads.take({ page_id, download_id });
    if (download.has_value())
        (void)Core::System::unlink(MUST(String::formatted("{}.part", download->string())));
    auto name = download.has_value() ? download->basename() : "file"sv;
    Application::the().display_error_dialog(MUST(String::formatted("Unable to download {}: {}", name, error)));
}

}
