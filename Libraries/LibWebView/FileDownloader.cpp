/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibHTTP/HeaderList.h>
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
        [this, request_id, destination = move(destination)](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const&, Optional<u32> response_code, Optional<String> const& reason_phrase, ReadonlyBytes payload) {
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

            if (auto result = save_file(destination, payload); result.is_error()) {
                auto error = MUST(String::formatted("Unable to save downloaded file file: {}", result.error()));
                Application::the().display_error_dialog(error);
            }

            // FIXME: Add a UI element (i.e. a download manager) to indicate download completion.
        });

    m_requests.set(request_id, request.release_nonnull());
}

}
