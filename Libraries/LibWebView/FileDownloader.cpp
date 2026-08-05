/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <LibCore/DirIterator.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/Loader/DownloadFilename.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/Application.h>
#include <LibWebView/DownloadSegmentation.h>
#include <LibWebView/DownloadStore.h>
#include <LibWebView/FileDownloader.h>

namespace WebView {

struct FileDownloader::Segment {
    u64 start_offset { 0 };
    u64 end_offset { NumericLimits<u64>::max() };
    u64 next_offset { 0 };

    RefPtr<Requests::Request> request;
    RefPtr<Core::Timer> retry_timer;

    // Request generations are download-wide so restarts cannot reuse an old generation.
    u64 request_generation { 0 };
    bool request_is_ranged { false };
    bool response_validated { false };
    u8 retry_count { 0 };

    MonotonicTime last_activity { MonotonicTime::now() };
    u8 stall_restart_count { 0 };

    bool is_complete() const { return next_offset > end_offset; }
    u64 downloaded_size() const { return next_offset - start_offset; }
    u64 remaining_size() const
    {
        if (is_complete())
            return 0;

        // An unbounded segment has all the bytes in the world left, and must not be allowed to wrap around to none.
        if (end_offset == NumericLimits<u64>::max())
            return NumericLimits<u64>::max();

        return end_offset - next_offset + 1;
    }
};

struct FileDownloader::ActiveDownload {
    ActiveDownload(NonnullOwnPtr<Core::File> file, LexicalPath temporary_destination)
        : file(move(file))
        , temporary_destination(move(temporary_destination))
    {
        segments.empend();
    }

    explicit ActiveDownload(LexicalPath temporary_destination)
        : temporary_destination(move(temporary_destination))
    {
    }

    Vector<Segment> segments;
    Function<void()> on_cancel;
    OwnPtr<Core::File> file;
    RefPtr<Core::Timer> stall_timer;

    // Segments share one file descriptor, so we track where it is currently positioned to avoid seeking when the
    // same segment writes twice in a row (which is every write for an unsegmented download). Empty means we do not
    // know where it is, and must seek before writing.
    Optional<u64> file_offset;

    LexicalPath temporary_destination;
    Core::ElapsedTimer progress_update_timer;
    Core::ElapsedTimer persist_timer;
    UnixDateTime created_time { UnixDateTime::now() };
    bool stopped { false };
    URL::URL effective_url;
    u32 redirect_count { 0 };
    bool can_issue_own_requests { false };
    bool can_restart_from_zero { false };
    DownloadRangeSupport range_support;
    bool segmentation_abandoned { false };
    bool restored_from_disk { false };

    u8 restart_count { 0 };
    u64 last_request_generation { 0 };
    Optional<MonotonicTime> rate_sample_time;
    u64 rate_sample_downloaded_size { 0 };
};

Optional<AK::Duration> FileDownloader::Download::estimated_time_remaining() const
{
    if (!bytes_per_second.has_value() || *bytes_per_second == 0)
        return {};
    if (!total_size.has_value() || downloaded_size >= *total_size)
        return {};

    auto remaining_size = *total_size - downloaded_size;
    return AK::Duration::from_seconds(static_cast<i64>(remaining_size / *bytes_per_second));
}

Optional<double> FileDownloader::Download::progress() const
{
    if (!total_size.has_value() || *total_size == 0)
        return {};

    return static_cast<double>(min(downloaded_size, *total_size)) / static_cast<double>(*total_size);
}

FileDownloader::FileDownloader() = default;
FileDownloader::~FileDownloader() = default;

static LexicalPath temporary_destination_for(LexicalPath const& destination, u64 download_id)
{
    auto suffix = ByteString::formatted(".{}.{}.download", download_id, generate_random_uuid());
    auto truncated_basename = Web::truncate_filename_to_byte_length(destination.basename(), Web::maximum_filename_byte_length - suffix.length());
    auto temporary_filename = ByteString::formatted("{}{}", truncated_basename, suffix);
    return LexicalPath::join(destination.dirname(), temporary_filename.view());
}

static constexpr u32 most_connections_we_are_willing_to_open = 16;

static u32 maximum_allowed_download_connections()
{
    return min(
        Application::settings().config_variable_as_u32(ConfigVariableID::MaximumConnectionsPerDownload),
        most_connections_we_are_willing_to_open);
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

u64 FileDownloader::download_file(IsPrivate is_private, URL::URL const& url, LexicalPath destination)
{
    auto download_id = start_download(is_private, url, move(destination));
    auto* active = active_download(download_id);
    if (!active)
        return download_id;

    active->can_issue_own_requests = true;
    active->can_restart_from_zero = true;
    start_download_request(download_id, url);

    return download_id;
}

void FileDownloader::start_download_request(u64 download_id, URL::URL const& url)
{
    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active)
        return;

    VERIFY(active->can_issue_own_requests);
    active->effective_url = url;

    // FIXME: What other request headers should be set? Perhaps we want to use exactly the same request headers used to
    //        originally fetch the image in WebContent.
    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });

    auto request = Application::request_server_client(download->is_private).start_request("GET"sv, url, *request_headers, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, {}, Requests::RequestClient::KeepAliveForTransfer::No, 0u);
    if (!request) {
        fail_download(download_id, "Unable to start request to download file"_string);
        return;
    }

    attach_request_to_download(download_id, 0, request.release_nonnull());
}

void FileDownloader::follow_download_redirect(u64 download_id, HTTP::HeaderList const& response_headers)
{
    static constexpr u32 maximum_redirects = 20;

    auto* active = active_download(download_id);
    if (!active)
        return;

    auto location = response_headers.get("Location"sv);
    if (!location.has_value()) {
        fail_download(download_id, "Received a redirect response without a location while downloading file"_string);
        return;
    }

    if (++active->redirect_count > maximum_redirects) {
        fail_download(download_id, "Received too many redirects while downloading file"_string);
        return;
    }

    auto redirect_url = active->effective_url.complete_url(*location);
    if (!redirect_url.has_value() || (redirect_url->scheme() != "http"sv && redirect_url->scheme() != "https"sv)) {
        fail_download(download_id, "Received an invalid redirect location while downloading file"_string);
        return;
    }

    stop_segment_request(*active, 0);

    start_download_request(download_id, redirect_url.release_value());
}

u64 FileDownloader::adopt_download(IsPrivate is_private, URL::URL const& url, LexicalPath destination, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ReadonlyBytes initial_data)
{
    auto download_id = start_download(is_private, url, move(destination), total_size);
    auto* active = active_download(download_id);
    if (!active)
        return download_id;

    active->effective_url = url;
    active->can_issue_own_requests = true;

    if (!initial_data.is_empty()) {
        append_download_data(download_id, initial_data);
        auto download = this->download(download_id);
        if (!download.has_value() || download->status != DownloadStatus::InProgress)
            return download_id;
    }

    auto request = Application::request_server_client(is_private).adopt_request(request_server_client_id, request_server_request_id);
    if (!request) {
        fail_download(download_id, "Unable to adopt request to download file"_string);
        return download_id;
    }

    attach_request_to_download(download_id, 0, request.release_nonnull());
    return download_id;
}

void FileDownloader::attach_request_to_download(u64 download_id, size_t segment_index, NonnullRefPtr<Requests::Request> request)
{
    auto* active = active_download(download_id);
    if (!active || segment_index >= active->segments.size())
        return;

    auto& segment = active->segments[segment_index];
    auto request_generation = ++active->last_request_generation;
    segment.request_generation = request_generation;
    segment.request = request;
    segment.response_validated = false;
    segment.last_activity = MonotonicTime::now();

    // Response data does not arrive in lockstep with response headers - it comes over its own pipe, which can be
    // ready before the headers message has been dispatched. Hold the body back until the response has been checked
    // over, so redirects and rejected ranges cannot write bytes that belong to a response we are about to discard.
    request->set_body_delivery_paused(true);

    request->set_unbuffered_request_callbacks(
        [this, download_id, segment_index, request_generation](NonnullRefPtr<HTTP::HeaderList> response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CameFromCache came_from_cache) {
            handle_segment_headers(download_id, segment_index, request_generation, *response_headers, response_code, reason_phrase, came_from_cache);
        },
        [this, download_id, segment_index, request_generation](Requests::ResponseData data) {
            append_segment_data(download_id, segment_index, data.bytes(), request_generation);
        },
        [](Core::ImmutableBytes) {
        },
        [this, download_id, segment_index, request_generation](u64 delivered_size, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            handle_segment_finished(download_id, segment_index, request_generation, delivered_size, network_error);
        });
}

void FileDownloader::handle_segment_headers(u64 download_id, size_t segment_index, u64 request_generation, HTTP::HeaderList const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Requests::CameFromCache came_from_cache)
{
    auto* download = mutable_download_or_null(download_id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    auto* active = active_download(download_id);
    if (!active || active->stopped || segment_index >= active->segments.size())
        return;

    auto& segment = active->segments[segment_index];
    if (segment.request_generation != request_generation)
        return;

    segment.last_activity = MonotonicTime::now();

    auto is_ranged = segment.request_is_ranged;

    if (active->can_issue_own_requests && !is_ranged && response_code.has_value() && Web::Fetch::Infrastructure::is_redirect_status(*response_code)) {
        follow_download_redirect(download_id, response_headers);
        return;
    }

    if (response_code.has_value() && *response_code >= 400) {
        if (is_ranged && *response_code == 416) {
            restart_download_from_zero(download_id, "The file on the server changed while it was being downloaded"_string);
            return;
        }

        if (response_is_rate_limited(response_code)) {
            handle_rate_limited_segment(download_id, segment_index, response_headers);
            return;
        }

        if (is_ranged) {
            auto is_worth_retrying = *response_code >= 500;
            if (is_worth_retrying && retry_segment_request(download_id, segment_index))
                return;

            abandon_segmentation(download_id);
            return;
        }

        fail_download(download_id, status_to_error_string({}, response_code, reason_phrase));
        return;
    }

    download->is_waiting_to_retry = false;

    if (is_ranged) {
        if (validate_range_response(download_id, segment_index, response_headers, response_code)) {
            segment.response_validated = true;
            segment.request->resume_body_delivery();
            maybe_redistribute_segments(download_id);
        }
        return;
    }

    if (auto extracted_length = response_headers.extract_length(); extracted_length.has<u64>())
        download->total_size = extracted_length.get<u64>();

    if (!active->can_issue_own_requests) {
        segment.response_validated = true;
        segment.request->resume_body_delivery();
        notify_download_updated(*download);
        return;
    }

    auto range_support = evaluate_range_support(response_headers, response_code, came_from_cache);

    // Splitting without validators is opt-in; resumption still needs an If-Range validator.
    auto may_use_range_support = range_support.validator.is_usable()
        || Application::settings().config_variable_as_bool(ConfigVariableID::SplitDownloadsWithoutValidators);

    if (range_support.supports_ranges && may_use_range_support) {
        active->range_support = move(range_support);
        download->can_resume = active->range_support.can_resume();

        maybe_split_download(download_id);
        persist_download_snapshot(download_id, PersistUrgency::Immediate);
    }

    // Splitting can reallocate the segment list or fail the download, so re-resolve afterwards.
    if (download->status != DownloadStatus::InProgress)
        return;

    if (auto& current_segment = active->segments[segment_index]; current_segment.request) {
        current_segment.response_validated = true;
        current_segment.request->resume_body_delivery();
    }
    notify_download_updated(*download);
}

bool FileDownloader::validate_range_response(u64 download_id, size_t segment_index, HTTP::HeaderList const& response_headers, Optional<u32> response_code)
{
    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active || segment_index >= active->segments.size())
        return false;

    auto const& segment = active->segments[segment_index];

    if (response_code != 206u) {
        abandon_segmentation(download_id);
        return false;
    }

    auto content_range = response_headers.extract_content_range_values();
    if (!content_range.has<HTTP::HeaderList::ContentRangeValues>()) {
        abandon_segmentation(download_id);
        return false;
    }

    // Never trust the offsets the server reports over the ones we asked for - we always write where we meant to. All
    // we do here is confirm that what is about to arrive is what we asked for.
    auto values = content_range.get<HTTP::HeaderList::ContentRangeValues>();
    if (values.first_byte_pos != segment.next_offset || values.last_byte_pos > segment.end_offset) {
        abandon_segmentation(download_id);
        return false;
    }
    if (values.complete_length.has_value() && download->total_size.has_value() && *values.complete_length != *download->total_size) {
        abandon_segmentation(download_id);
        return false;
    }

    auto validator = evaluate_range_support(response_headers, response_code, Requests::CameFromCache::No).validator;
    if (!active->range_support.validator.matches(validator)) {
        abandon_segmentation(download_id);
        return false;
    }

    return true;
}

void FileDownloader::handle_segment_finished(u64 download_id, size_t segment_index, u64 request_generation, u64 delivered_size, Optional<Requests::NetworkError> const& network_error)
{
    auto* download = mutable_download_or_null(download_id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    auto* active = active_download(download_id);
    if (!active || active->stopped || segment_index >= active->segments.size())
        return;

    if (active->segments[segment_index].request_generation != request_generation)
        return;

    auto is_segmented = active->segments.size() > 1 || active->segments[segment_index].request_is_ranged;
    active->segments[segment_index].request = nullptr;

    if (!is_segmented) {
        download->total_size = delivered_size;

        if (network_error.has_value())
            fail_download(download_id, status_to_error_string(network_error, {}, {}));
        else
            finish_download(download_id);

        return;
    }

    if (network_error.has_value() || !active->segments[segment_index].is_complete()) {
        if (retry_segment_request(download_id, segment_index))
            return;

        abandon_segmentation(download_id);
        return;
    }

    refresh_download_progress(*download, *active);
    persist_download_snapshot(download_id, PersistUrgency::Immediate);
    notify_download_updated(*download);

    maybe_finish_download(download_id);
}

void FileDownloader::maybe_finish_download(u64 download_id)
{
    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active)
        return;

    for (auto const& segment : active->segments) {
        if (!segment.is_complete())
            return;
    }

    finish_download(download_id);
}

void FileDownloader::maybe_split_download(u64 download_id)
{
    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active)
        return;

    if (active->segmentation_abandoned || active->segments.size() != 1 || !download->total_size.has_value())
        return;

    DownloadSegmentationParameters parameters;
    parameters.maximum_connections = maximum_allowed_download_connections();

    auto plan = plan_download_segments(*download->total_size, active->segments[0].next_offset, parameters);
    if (plan.is_empty())
        return;

    if (active->file)
        (void)active->file->truncate(*download->total_size);

    active->segments[0].start_offset = plan[0].start_offset;
    active->segments[0].end_offset = plan[0].end_offset;

    for (size_t i = 1; i < plan.size(); ++i) {
        active->segments.empend();

        auto& segment = active->segments.last();
        segment.start_offset = plan[i].start_offset;
        segment.end_offset = plan[i].end_offset;
        segment.next_offset = plan[i].start_offset;
    }

    for (size_t i = 1; i < plan.size(); ++i)
        start_segment_request(download_id, i);

    start_stall_watchdog(download_id);
    refresh_download_progress(*download, *active);
}

void FileDownloader::start_segment_request(u64 download_id, size_t segment_index)
{
    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || download->status != DownloadStatus::InProgress || !active || active->stopped)
        return;
    if (segment_index >= active->segments.size())
        return;

    VERIFY(active->can_issue_own_requests);

    auto& segment = active->segments[segment_index];
    if (segment.is_complete())
        return;

    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });
    auto range = segment.end_offset == NumericLimits<u64>::max()
        ? ByteString::formatted("bytes={}-", segment.next_offset)
        : ByteString::formatted("bytes={}-{}", segment.next_offset, segment.end_offset);
    request_headers->set({ "Range"sv, move(range) });

    if (auto if_range = active->range_support.validator.if_range_value(); if_range.has_value())
        request_headers->set({ "If-Range"sv, if_range->to_byte_string() });

    segment.request_is_ranged = true;

    auto request = Application::request_server_client(download->is_private).start_request("GET"sv, active->effective_url, *request_headers, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::Yes, {}, Requests::RequestClient::KeepAliveForTransfer::No, static_cast<u32>(segment_index));
    if (!request) {
        fail_download(download_id, "Unable to start request to download file"_string);
        return;
    }

    attach_request_to_download(download_id, segment_index, request.release_nonnull());
    refresh_download_progress(*download, *active);
}

void FileDownloader::maybe_redistribute_segments(u64 download_id)
{
    static constexpr u64 minimum_size_to_steal = 2 * DownloadSegmentationParameters {}.minimum_segment_size;

    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || download->status != DownloadStatus::InProgress || !active || active->stopped)
        return;
    if (active->segmentation_abandoned || active->segments.size() < 2 || !active->can_issue_own_requests)
        return;
    if (!active->range_support.supports_ranges)
        return;

    size_t live_requests = 0;
    for (auto const& segment : active->segments) {
        if (segment.request && !segment.is_complete())
            ++live_requests;
    }
    if (live_requests >= maximum_allowed_download_connections())
        return;

    Optional<size_t> donor_index;
    u64 donor_remaining = 0;

    for (size_t i = 0; i < active->segments.size(); ++i) {
        auto const& segment = active->segments[i];
        if (segment.is_complete() || segment.end_offset == NumericLimits<u64>::max())
            continue;

        // Do not shrink a segment while its current Content-Range is still unvalidated.
        if (segment.request && !segment.response_validated)
            continue;

        if (segment.remaining_size() > donor_remaining) {
            donor_remaining = segment.remaining_size();
            donor_index = i;
        }
    }

    if (!donor_index.has_value() || donor_remaining < minimum_size_to_steal)
        return;

    auto stolen_start = active->segments[*donor_index].next_offset + (donor_remaining - donor_remaining / 2);
    auto stolen_end = active->segments[*donor_index].end_offset;
    active->segments[*donor_index].end_offset = stolen_start - 1;

    active->segments.empend();

    auto& stolen = active->segments.last();
    stolen.start_offset = stolen_start;
    stolen.end_offset = stolen_end;
    stolen.next_offset = stolen_start;

    start_segment_request(download_id, active->segments.size() - 1);
    persist_download_snapshot(download_id, PersistUrgency::Immediate);
}

void FileDownloader::start_stall_watchdog(u64 download_id)
{
    static constexpr int stall_check_interval_ms = 1000;

    auto* active = active_download(download_id);
    if (!active)
        return;

    if (!active->stall_timer) {
        active->stall_timer = Core::Timer::create_repeating(stall_check_interval_ms, [this, download_id] {
            check_for_stalled_segments(download_id);
        });
    }

    active->stall_timer->start();
}

void FileDownloader::check_for_stalled_segments(u64 download_id)
{
    static constexpr auto stalled_connection_timeout = AK::Duration::from_seconds(5);
    static constexpr u8 maximum_stall_restarts = 4;

    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active)
        return;

    if (download->status != DownloadStatus::InProgress || active->stopped || active->segments.size() <= 1) {
        // Keep the timer alive when stopping from inside its callback.
        active->stall_timer->stop();
        return;
    }

    if (!Application::settings().config_variable_as_bool(ConfigVariableID::RestartStalledConnections))
        return;

    auto now = MonotonicTime::now();

    for (size_t segment_index = 0; segment_index < active->segments.size(); ++segment_index) {
        auto& segment = active->segments[segment_index];

        if (segment.is_complete() || !segment.request)
            continue;
        if (now - segment.last_activity < stalled_connection_timeout)
            continue;

        if (segment.stall_restart_count >= maximum_stall_restarts) {
            fail_or_pause_download(download_id, "The download stalled and could not be revived"_string);
            return;
        }

        restart_stalled_segment(download_id, segment_index);

        download = mutable_download_or_null(download_id);
        active = active_download(download_id);
        if (!download || download->status != DownloadStatus::InProgress || !active)
            return;
    }
}

void FileDownloader::restart_stalled_segment(u64 download_id, size_t segment_index)
{
    auto* active = active_download(download_id);
    if (!active || segment_index >= active->segments.size())
        return;

    auto& segment = active->segments[segment_index];

    auto delay_seconds = segment.stall_restart_count == 0 ? 0 : 1 << (segment.stall_restart_count - 1);
    ++segment.stall_restart_count;

    stop_segment_request(*active, segment_index);
    segment.last_activity = MonotonicTime::now();

    if (delay_seconds == 0) {
        start_segment_request(download_id, segment_index);
        return;
    }

    segment.retry_timer = Core::Timer::create_single_shot(delay_seconds * 1000, [this, download_id, segment_index] {
        auto* active = active_download(download_id);
        if (!active || active->stopped || segment_index >= active->segments.size())
            return;

        active->segments[segment_index].retry_timer = nullptr;
        start_segment_request(download_id, segment_index);
    });
    segment.retry_timer->start();
}

bool FileDownloader::retry_segment_request(u64 download_id, size_t segment_index, AK::Duration delay)
{
    static constexpr u8 maximum_segment_retries = 5;

    auto* active = active_download(download_id);
    if (!active || segment_index >= active->segments.size())
        return false;

    auto& segment = active->segments[segment_index];
    if (segment.is_complete())
        return false;
    if (++segment.retry_count > maximum_segment_retries)
        return false;

    stop_segment_request(*active, segment_index);

    auto is_initial_request = segment_index == 0 && !segment.request_is_ranged;

    if (delay.is_zero()) {
        if (is_initial_request)
            start_download_request(download_id, active->effective_url);
        else
            start_segment_request(download_id, segment_index);

        return true;
    }

    segment.retry_timer = Core::Timer::create_single_shot(static_cast<int>(delay.to_milliseconds()), [this, download_id, segment_index, is_initial_request] {
        auto* active = active_download(download_id);
        if (!active || active->stopped || segment_index >= active->segments.size())
            return;

        active->segments[segment_index].retry_timer = nullptr;

        if (is_initial_request)
            start_download_request(download_id, active->effective_url);
        else
            start_segment_request(download_id, segment_index);
    });
    segment.retry_timer->start();

    return true;
}

void FileDownloader::handle_rate_limited_segment(u64 download_id, size_t segment_index, HTTP::HeaderList const& response_headers)
{
    static constexpr auto minimum_retry_delay = AK::Duration::from_seconds(1);
    static constexpr auto maximum_retry_delay = AK::Duration::from_seconds(60);

    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active || segment_index >= active->segments.size())
        return;

    active->segmentation_abandoned = true;

    if (active->segments.size() > 1) {
        abandon_segmentation(download_id);
        return;
    }

    auto delay = parse_retry_after(response_headers).value_or_lazy_evaluated([&] {
        auto backoff = 1u << min(active->segments[segment_index].retry_count, static_cast<u8>(5));
        return AK::Duration::from_seconds(minimum_retry_delay.to_seconds() * backoff);
    });
    delay = min(delay, maximum_retry_delay);

    if (!retry_segment_request(download_id, segment_index, delay)) {
        fail_download(download_id, "The server is refusing further requests for this file"_string);
        return;
    }

    download->is_waiting_to_retry = true;
    download->bytes_per_second = {};
    active->rate_sample_time = {};

    refresh_download_progress(*download, *active);
    notify_download_updated(*download);
}

void FileDownloader::abandon_segmentation(u64 download_id)
{
    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active || active->segments.is_empty())
        return;

    active->segmentation_abandoned = true;
    active->range_support = {};
    download->can_resume = false;
    forget_persisted_download(download_id);

    auto& first_segment = active->segments[0];
    if (first_segment.request && !first_segment.request_is_ranged) {
        for (size_t i = active->segments.size(); i > 1; --i)
            stop_segment_request(*active, i - 1);

        active->segments.shrink(1);
        active->segments[0].end_offset = download->total_size.has_value()
            ? *download->total_size - 1
            : NumericLimits<u64>::max();

        refresh_download_progress(*download, *active);
        notify_download_updated(*download);
        return;
    }

    restart_download_from_zero(download_id, "The server would not serve the rest of this file"_string);
}

void FileDownloader::restart_download_from_zero(u64 download_id, String reason_if_exhausted)
{
    static constexpr u8 maximum_download_restarts = 1;

    auto* download = mutable_download_or_null(download_id);
    auto* active = active_download(download_id);
    if (!download || !active)
        return;

    if (!active->can_restart_from_zero) {
        fail_download(download_id, move(reason_if_exhausted));
        return;
    }

    if (++active->restart_count > maximum_download_restarts) {
        fail_download(download_id, move(reason_if_exhausted));
        return;
    }

    for (size_t i = active->segments.size(); i > 0; --i)
        stop_segment_request(*active, i - 1);

    active->segments.clear();
    active->segments.empend();
    active->range_support = {};

    download->can_resume = false;
    download->total_size = {};
    forget_persisted_download(download_id);

    if (active->file) {
        // Truncating leaves the descriptor wherever it was, so the next write has to seek rather than trust where it
        // thinks it is.
        active->file_offset = {};

        if (auto result = active->file->truncate(0); result.is_error()) {
            fail_download(download_id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
            return;
        }
    }

    refresh_download_progress(*download, *active);
    start_download_request(download_id, active->effective_url);
}

u64 FileDownloader::start_download(IsPrivate is_private, URL::URL const& url, LexicalPath destination, Optional<u64> total_size)
{
    auto download_id = m_next_download_id++;

    m_downloads.append(Download {
        .id = download_id,
        .is_private = is_private,
        .url = url,
        .destination = move(destination),
        .total_size = total_size,
        .error = {},
        .bytes_per_second = {},
        .created_time = UnixDateTime::now(),
    });
    auto& download = m_downloads.last();
    notify_download_added(download);

    auto temporary_destination = temporary_destination_for(download.destination, download_id);
    auto file_or_error = Core::File::open(temporary_destination.string(), Core::File::OpenMode::ReadWrite | Core::File::OpenMode::MustBeNew);
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

bool FileDownloader::has_unresumable_downloads() const
{
    for (auto const& download : m_downloads) {
        if (download.status == DownloadStatus::InProgress && !download.can_resume)
            return true;
    }

    return false;
}

void FileDownloader::append_download_data(u64 id, ReadonlyBytes bytes)
{
    append_segment_data(id, 0, bytes);
}

void FileDownloader::append_segment_data(u64 id, size_t segment_index, ReadonlyBytes bytes, Optional<u64> request_generation)
{
    constexpr i64 progress_update_interval_ms = 100;

    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::InProgress)
        return;

    auto* active = active_download(id);
    if (!active || active->stopped || segment_index >= active->segments.size())
        return;

    auto& segment = active->segments[segment_index];
    if (request_generation.has_value() && segment.request_generation != *request_generation)
        return;

    segment.last_activity = MonotonicTime::now();
    segment.stall_restart_count = 0;

    if (bytes.size() > segment.remaining_size())
        bytes = bytes.trim(segment.remaining_size());
    if (bytes.is_empty())
        return;

    if (auto result = write_segment_data(*active, segment, bytes); result.is_error()) {
        fail_or_pause_download(id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
        return;
    }

    refresh_download_progress(*download, *active);

    if (segment.is_complete()) {
        stop_segment_request(*active, segment_index);
        maybe_redistribute_segments(id);

        download = mutable_download_or_null(id);
        active = active_download(id);
        if (!download || download->status != DownloadStatus::InProgress || !active)
            return;

        refresh_download_progress(*download, *active);

        notify_download_updated(*download);
        maybe_finish_download(id);
        return;
    }

    if (active->progress_update_timer.is_valid() && active->progress_update_timer.elapsed_milliseconds() < progress_update_interval_ms)
        return;

    active->progress_update_timer.start();
    sample_download_rate(*download, *active);
    persist_download_snapshot(id, PersistUrgency::Throttled);
    notify_download_updated(*download);
}

ErrorOr<void> FileDownloader::write_segment_data(ActiveDownload& active, Segment& segment, ReadonlyBytes bytes)
{
    VERIFY(active.file);

    if (active.file_offset != segment.next_offset) {
        active.file_offset = {};
        TRY(active.file->seek(static_cast<i64>(segment.next_offset), SeekMode::SetPosition));
        active.file_offset = segment.next_offset;
    }

    // If the write fails part way through we have no idea how much of it landed, so the position is only known again
    // once it has succeeded outright.
    active.file_offset = {};
    TRY(active.file->write_until_depleted(bytes));

    segment.next_offset += bytes.size();
    active.file_offset = segment.next_offset;

    return {};
}

void FileDownloader::sample_download_rate(Download& download, ActiveDownload& active)
{
    static constexpr auto rate_sample_interval = AK::Duration::from_milliseconds(500);
    static constexpr double rate_sample_weight = 0.3;

    auto now = MonotonicTime::now();

    if (!active.rate_sample_time.has_value()) {
        active.rate_sample_time = now;
        active.rate_sample_downloaded_size = download.downloaded_size;
        return;
    }

    auto elapsed = now - *active.rate_sample_time;
    if (elapsed < rate_sample_interval)
        return;

    auto downloaded_since_sample = download.downloaded_size > active.rate_sample_downloaded_size
        ? download.downloaded_size - active.rate_sample_downloaded_size
        : 0;

    auto sampled_rate = static_cast<double>(downloaded_since_sample) * 1000.0 / static_cast<double>(elapsed.to_milliseconds());

    download.bytes_per_second = download.bytes_per_second.has_value()
        ? static_cast<u64>((1.0 - rate_sample_weight) * static_cast<double>(*download.bytes_per_second) + rate_sample_weight * sampled_rate)
        : static_cast<u64>(sampled_rate);

    active.rate_sample_time = now;
    active.rate_sample_downloaded_size = download.downloaded_size;
}

void FileDownloader::refresh_download_progress(Download& download, ActiveDownload const& active)
{
    u64 downloaded_size = 0;
    u32 connection_count = 0;

    for (auto const& segment : active.segments) {
        downloaded_size += segment.downloaded_size();
        if (segment.request)
            ++connection_count;
    }

    download.downloaded_size = downloaded_size;
    download.connection_count = connection_count;
}

void FileDownloader::stop_segment_request(ActiveDownload& active, size_t segment_index)
{
    if (segment_index >= active.segments.size())
        return;

    auto& segment = active.segments[segment_index];
    segment.retry_timer = nullptr;

    if (!segment.request)
        return;

    segment.request->stop();
    segment.request = nullptr;
    ++segment.request_generation;
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

    if (auto result = FileSystem::move_file(download->destination.string(), active->temporary_destination.string()); result.is_error()) {
        fail_download(id, MUST(String::formatted("Unable to save downloaded file: {}", result.error())));
        return;
    }

    forget_persisted_download(id);

    download->status = DownloadStatus::Completed;
    download->bytes_per_second = {};
    if (!download->total_size.has_value())
        download->total_size = download->downloaded_size;
    notify_download_updated(*download);

    Core::deferred_invoke([this, id] {
        m_active_downloads.remove(id);
    });
}

void FileDownloader::pause_download(u64 id)
{
    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::InProgress || !download->can_resume)
        return;

    auto* active = active_download(id);
    if (!active)
        return;

    for (size_t i = active->segments.size(); i > 0; --i)
        stop_segment_request(*active, i - 1);

    active->file = nullptr;
    active->file_offset = {};

    download->status = DownloadStatus::Paused;
    download->is_waiting_to_retry = false;
    active->rate_sample_time = {};

    refresh_download_progress(*download, *active);
    persist_download_snapshot(id, PersistUrgency::Immediate);
    notify_download_updated(*download);
}

void FileDownloader::resume_download(u64 id)
{
    // A partial file may be ahead of the offsets we recorded for it - the bytes we wrote last are only in the page
    // cache until the operating system gets around to them, so a machine that lost power has fewer of them than we
    // think. Re-downloading the tail end of each segment costs nothing, as writing a byte range twice produces the
    // same file either way.
    static constexpr u64 resume_rewind_margin = 1 * MiB;

    auto* download = mutable_download_or_null(id);
    if (!download || download->status != DownloadStatus::Paused)
        return;

    auto* active = active_download(id);
    if (!active)
        return;

    if (!active->file) {
        auto file_or_error = Core::File::open(active->temporary_destination.string(), Core::File::OpenMode::ReadWrite);
        if (file_or_error.is_error()) {
            fail_download(id, MUST(String::formatted("Unable to resume downloading file: {}", file_or_error.error())));
            return;
        }

        active->file = file_or_error.release_value();
        active->file_offset = {};
    }

    u64 required_size = 0;
    for (auto const& segment : active->segments)
        required_size = max(required_size, segment.next_offset);

    auto size_or_error = active->file->size();
    if (size_or_error.is_error() || size_or_error.value() < required_size) {
        download->status = DownloadStatus::InProgress;
        restart_download_from_zero(id, "The partially downloaded file is no longer usable"_string);
        return;
    }

    if (active->restored_from_disk) {
        active->restored_from_disk = false;

        for (auto& segment : active->segments)
            segment.next_offset -= min(segment.downloaded_size(), resume_rewind_margin);
    }

    if (download->total_size.has_value()) {
        for (auto& segment : active->segments) {
            if (segment.end_offset == NumericLimits<u64>::max())
                segment.end_offset = *download->total_size - 1;
        }
    }

    download->status = DownloadStatus::InProgress;
    download->error = {};
    active->rate_sample_time = {};

    for (size_t i = 0; i < active->segments.size(); ++i) {
        auto& segment = active->segments[i];
        segment.retry_count = 0;
        segment.stall_restart_count = 0;
        start_segment_request(id, i);
    }

    if (active->segments.size() > 1)
        start_stall_watchdog(id);

    refresh_download_progress(*download, *active);
    persist_download_snapshot(id, PersistUrgency::Immediate);
    notify_download_updated(*download);

    maybe_finish_download(id);
}

void FileDownloader::pause_active_downloads()
{
    Vector<u64> resumable_download_ids;
    for (auto const& download : m_downloads) {
        if (download.status == DownloadStatus::InProgress && download.can_resume)
            resumable_download_ids.append(download.id);
    }

    for (auto id : resumable_download_ids)
        pause_download(id);
}

void FileDownloader::fail_or_pause_download(u64 id, String error)
{
    auto* download = mutable_download_or_null(id);

    if (download && download->status == DownloadStatus::InProgress && download->can_resume) {
        pause_download(id);

        if (download->status == DownloadStatus::Paused) {
            download->error = move(error);
            notify_download_updated(*download);
            return;
        }
    }

    fail_download(id, move(error));
}

void FileDownloader::adopt_download_store(Badge<Application>, DownloadStore& download_store)
{
    m_download_store = &download_store;

    m_next_download_id = max(m_next_download_id, download_store.maximum_download_id() + 1);

    restore_persisted_downloads();
}

void FileDownloader::persist_download_snapshot(u64 id, PersistUrgency urgency)
{
    static constexpr i64 persist_interval_ms = 1000;

    if (!m_download_store)
        return;

    auto* download = mutable_download_or_null(id);
    auto* active = active_download(id);
    if (!download || !active)
        return;

    if (download->is_private == IsPrivate::Yes)
        return;

    if (!download->can_resume || !download->total_size.has_value() || !status_is_active(download->status))
        return;

    if (urgency == PersistUrgency::Throttled && active->persist_timer.is_valid() && active->persist_timer.elapsed_milliseconds() < persist_interval_ms)
        return;

    active->persist_timer.start();

    DownloadRecord record {
        .id = download->id,
        .url = active->effective_url.serialize(),
        .display_url = download->url.serialize(),
        .destination = String::from_byte_string(download->destination.string()).release_value_but_fixme_should_propagate_errors(),
        .temporary_destination = String::from_byte_string(active->temporary_destination.string()).release_value_but_fixme_should_propagate_errors(),
        .total_size = *download->total_size,
        .etag = active->range_support.validator.etag,
        .last_modified = active->range_support.validator.last_modified,
        .segments = {},
        .created_time = active->created_time,
        .can_restart_from_zero = active->can_restart_from_zero,
    };

    record.segments.ensure_capacity(active->segments.size());
    for (auto const& segment : active->segments)
        record.segments.unchecked_append({ segment.start_offset, segment.end_offset, segment.next_offset });

    quick_sort(record.segments, [](auto const& a, auto const& b) { return a.start_offset < b.start_offset; });

    m_download_store->save_download(record);
}

void FileDownloader::forget_persisted_download(u64 id)
{
    if (m_download_store)
        m_download_store->remove_download(id);
}

void FileDownloader::restore_persisted_downloads()
{
    VERIFY(m_download_store);

    HashTable<ByteString> directories;
    HashTable<ByteString> temporary_paths_in_use;

    directories.set(Core::StandardPaths::downloads_directory());

    for (auto& record : m_download_store->resumable_downloads()) {
        LexicalPath temporary_destination { record.temporary_destination.to_byte_string() };
        directories.set(temporary_destination.dirname());

        if (restore_persisted_download(record)) {
            temporary_paths_in_use.set(temporary_destination.string());
            continue;
        }

        m_download_store->remove_download(record.id);
        (void)FileSystem::remove(temporary_destination.string(), FileSystem::RecursionMode::Disallowed);
    }

    remove_orphaned_temporary_files(directories, temporary_paths_in_use);
}

bool FileDownloader::restore_persisted_download(DownloadRecord& record)
{
    if (record.total_size == 0 || record.segments.is_empty())
        return false;

    if (record.segments.last().end_offset + 1 != record.total_size)
        return false;

    // The persisted segments must tile [0, total_size) in order, with each segment's resume point inside its own range.
    u64 highest_next_offset = 0;
    u64 expected_start_offset = 0;
    for (auto const& segment : record.segments) {
        if (segment.start_offset != expected_start_offset)
            return false;
        if (segment.end_offset < segment.start_offset || segment.end_offset >= record.total_size)
            return false;
        if (segment.next_offset < segment.start_offset || segment.next_offset > segment.end_offset + 1)
            return false;

        expected_start_offset = segment.end_offset + 1;
        highest_next_offset = max(highest_next_offset, segment.next_offset);
    }

    auto url = URL::Parser::basic_parse(record.display_url);
    auto effective_url = URL::Parser::basic_parse(record.url);
    if (!url.has_value() || !effective_url.has_value())
        return false;

    LexicalPath temporary_destination { record.temporary_destination.to_byte_string() };

    auto size = FileSystem::size_from_stat(temporary_destination.string());
    if (size.is_error() || static_cast<u64>(size.value()) < highest_next_offset)
        return false;

    m_downloads.append(Download {
        .id = record.id,
        .is_private = IsPrivate::No,
        .url = url.release_value(),
        .destination = LexicalPath { record.destination.to_byte_string() },
        .status = DownloadStatus::Paused,
        .total_size = record.total_size,
        .error = {},
        .can_resume = true,
        .bytes_per_second = {},
        .created_time = record.created_time,
    });

    auto active = make<ActiveDownload>(move(temporary_destination));
    active->effective_url = effective_url.release_value();
    active->can_issue_own_requests = true;
    active->can_restart_from_zero = record.can_restart_from_zero;
    active->created_time = record.created_time;
    active->restored_from_disk = true;
    active->range_support.supports_ranges = true;
    active->range_support.total_size = record.total_size;
    active->range_support.validator.etag = move(record.etag);
    active->range_support.validator.last_modified = move(record.last_modified);

    active->segments.ensure_capacity(record.segments.size());
    for (auto const& segment : record.segments) {
        active->segments.empend();

        auto& restored_segment = active->segments.last();
        restored_segment.start_offset = segment.start_offset;
        restored_segment.end_offset = segment.end_offset;
        restored_segment.next_offset = segment.next_offset;
    }

    auto& download = m_downloads.last();
    refresh_download_progress(download, *active);

    m_active_downloads.set(record.id, move(active));
    notify_download_added(download);

    return true;
}

void FileDownloader::remove_orphaned_temporary_files(HashTable<ByteString> const& directories, HashTable<ByteString> const& temporary_paths_in_use)
{
    // Another Ladybird may well be downloading right now, and its partial files look exactly like ours. Only sweep
    // files old enough that nothing could plausibly still be writing to them.
    static constexpr auto minimum_orphan_age = AK::Duration::from_seconds(24 * 60 * 60);

    auto is_temporary_download_filename = [](StringView filename) {
        auto parts = filename.split_view('.');
        if (parts.size() < 4 || parts.take_last() != "download"sv)
            return false;

        auto uuid = parts.take_last();
        if (uuid.length() != 36)
            return false;

        auto download_id = parts.take_last();
        return !download_id.is_empty() && all_of(download_id, is_ascii_digit);
    };

    auto now = UnixDateTime::now();

    for (auto const& directory : directories) {
        Core::DirIterator iterator { directory, Core::DirIterator::SkipParentAndBaseDir };

        while (iterator.has_next()) {
            auto path = iterator.next_full_path();
            if (!is_temporary_download_filename(LexicalPath::basename(path)))
                continue;
            if (temporary_paths_in_use.contains(path))
                continue;

            auto stat = Core::File::stat(path);
            if (stat.is_error())
                continue;
            if (now - UnixDateTime::from_seconds_since_epoch(stat.value().st_mtime) < minimum_orphan_age)
                continue;

            (void)FileSystem::remove(path, FileSystem::RecursionMode::Disallowed);
        }
    }
}

Optional<FileDownloader::Download const&> FileDownloader::download(u64 id) const
{
    for (auto const& download : m_downloads) {
        if (download.id == id)
            return download;
    }
    return {};
}

Vector<FileDownloader::SegmentProgress> FileDownloader::segment_progress(u64 id) const
{
    auto active = m_active_downloads.get(id);
    if (!active.has_value() || (*active)->segments.size() < 2)
        return {};

    Vector<SegmentProgress> progress;
    progress.ensure_capacity((*active)->segments.size());

    for (auto const& segment : (*active)->segments)
        progress.unchecked_append({ segment.start_offset, segment.end_offset, segment.next_offset });

    quick_sort(progress, [](auto const& a, auto const& b) { return a.start_offset < b.start_offset; });

    return progress;
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
    for (size_t segment_index = 0; segment_index < active->segments.size(); ++segment_index)
        stop_segment_request(*active, segment_index);
    if (active->on_cancel)
        active->on_cancel();

    active->file = nullptr;
    (void)FileSystem::remove(active->temporary_destination.string(), FileSystem::RecursionMode::Disallowed);

    Core::deferred_invoke([this, id] {
        m_active_downloads.remove(id);
    });
}

void FileDownloader::cancel_unresumable_downloads()
{
    Vector<u64> unresumable_download_ids;
    for (auto const& download : m_downloads) {
        if (download.status == DownloadStatus::InProgress && !download.can_resume)
            unresumable_download_ids.append(download.id);
    }

    for (auto id : unresumable_download_ids)
        cancel_download(id);
}

void FileDownloader::cancel_private_downloads()
{
    for (size_t i = m_downloads.size(); i > 0; --i) {
        auto const& download = m_downloads[i - 1];

        if (!status_is_active(download.status))
            continue;
        if (download.is_private == IsPrivate::No)
            continue;

        auto id = download.id;
        cancel_download(id);

        m_downloads.remove(i - 1);
        notify_download_removed(id);
    }
}

void FileDownloader::cancel_download(u64 id)
{
    auto* download = mutable_download_or_null(id);
    if (!download || !status_is_active(download->status))
        return;

    download->status = DownloadStatus::Canceled;
    download->error = {};
    forget_persisted_download(id);

    discard_active_download(id);

    notify_download_updated(*download);
}

void FileDownloader::fail_download(u64 id, String error)
{
    auto* download = mutable_download_or_null(id);
    if (!download || !status_is_active(download->status))
        return;

    download->status = DownloadStatus::Failed;
    download->error = move(error);
    forget_persisted_download(id);

    discard_active_download(id);

    notify_download_updated(*download);
}

Vector<u64> FileDownloader::prune_inactive_downloads()
{
    return remove_inactive_downloads_created_since(UnixDateTime::earliest());
}

Vector<u64> FileDownloader::remove_inactive_downloads_created_since(UnixDateTime since)
{
    Vector<u64> removed_download_ids;

    for (size_t i = m_downloads.size(); i > 0; --i) {
        auto const index = i - 1;
        auto const id = m_downloads[index].id;
        if (status_is_active(m_downloads[index].status))
            continue;
        if (m_downloads[index].created_time < since)
            continue;

        forget_persisted_download(id);
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
