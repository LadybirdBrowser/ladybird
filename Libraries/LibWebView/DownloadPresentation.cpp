/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibWebView/DownloadPresentation.h>

namespace WebView {

static constexpr auto max_visible_downloads_in_popover = 5uz;

static String download_count_text(u64 count, StringView singular, StringView plural)
{
    return MUST(String::formatted("{} {}", count, count == 1 ? singular : plural));
}

static String download_percent_text(double progress)
{
    if (progress < 0.0)
        progress = 0.0;
    if (progress > 1.0)
        progress = 1.0;

    return MUST(String::formatted("{}%", static_cast<int>(progress * 100.0)));
}

struct SizeUnit {
    u64 divisor;
    StringView suffix;
};

// Sizes are given in the decimal units the system's file browser shows them in.
static SizeUnit unit_for_download_size(u64 size)
{
    static constexpr u64 kilobyte = 1000;
    static constexpr u64 megabyte = kilobyte * 1000;
    static constexpr u64 gigabyte = megabyte * 1000;
    static constexpr u64 terabyte = gigabyte * 1000;

    if (size < kilobyte)
        return { 1, "bytes"sv };
    if (size < megabyte)
        return { kilobyte, "KB"sv };
    if (size < gigabyte)
        return { megabyte, "MB"sv };
    if (size < terabyte)
        return { gigabyte, "GB"sv };
    return { terabyte, "TB"sv };
}

static String unitless_download_size_text(u64 size, SizeUnit unit)
{
    if (unit.divisor == 1)
        return MUST(String::formatted("{}", size));

    return MUST(String::formatted("{:.1f}", static_cast<double>(size) / static_cast<double>(unit.divisor)));
}

static String download_size_text(u64 size)
{
    auto unit = unit_for_download_size(size);
    return MUST(String::formatted("{} {}", unitless_download_size_text(size, unit), unit.suffix));
}

String download_time_remaining_text(AK::Duration time_remaining)
{
    auto seconds = time_remaining.to_seconds();

    if (seconds < 60)
        return MUST(String::formatted("{} left", download_count_text(max(seconds, static_cast<i64>(1)), "second"sv, "seconds"sv)));

    auto total_minutes = (seconds + 59) / 60;
    if (total_minutes < 60)
        return MUST(String::formatted("{} left", download_count_text(total_minutes, "minute"sv, "minutes"sv)));

    auto hours = total_minutes / 60;
    auto minutes = total_minutes % 60;
    if (minutes == 0)
        return MUST(String::formatted("{} left", download_count_text(hours, "hour"sv, "hours"sv)));
    return MUST(String::formatted("{} {} left", download_count_text(hours, "hour"sv, "hours"sv), download_count_text(minutes, "minute"sv, "minutes"sv)));
}

String download_status_text(FileDownloader::Download const& download)
{
    using DownloadStatus = FileDownloader::DownloadStatus;

    // The progress bar beside the status shows how far along a download is, so the status says only how much of
    // it has arrived, such as "12.4 of 19.3 MB".
    auto downloaded_size_text = [&]() {
        if (download.total_size.has_value() && *download.total_size > 0) {
            auto unit = unit_for_download_size(*download.total_size);
            return MUST(String::formatted("{} of {} {}",
                unitless_download_size_text(min(download.downloaded_size, *download.total_size), unit),
                unitless_download_size_text(*download.total_size, unit),
                unit.suffix));
        }
        return MUST(String::formatted("{} downloaded", download_size_text(download.downloaded_size)));
    };

    switch (download.status) {
    case DownloadStatus::InProgress: {
        if (download.is_waiting_to_retry)
            return MUST(String::formatted("Waiting to retry · {}", downloaded_size_text()));

        StringBuilder builder;
        builder.append(downloaded_size_text());
        if (auto time_remaining = download.estimated_time_remaining(); time_remaining.has_value())
            builder.appendff(" · {}", download_time_remaining_text(*time_remaining));

        return MUST(builder.to_string());
    }
    case DownloadStatus::Paused:
        if (download.error.has_value() && !download.error->is_empty())
            return MUST(String::formatted("Paused · {}", *download.error));
        return MUST(String::formatted("Paused · {}", downloaded_size_text()));
    case DownloadStatus::Completed:
        // NB: A finished download is the one with nothing else to say, so its status is just its size.
        return download_size_text(download.downloaded_size);
    case DownloadStatus::Canceled:
        return "Canceled"_string;
    case DownloadStatus::Failed:
        if (download.error.has_value() && !download.error->is_empty())
            return MUST(String::formatted("Failed · {}", *download.error));
        return "Failed"_string;
    }
    VERIFY_NOT_REACHED();
}

Vector<FileDownloader::Download const*> recent_downloads_for_popover(ReadonlySpan<FileDownloader::Download> downloads)
{
    Vector<FileDownloader::Download const*> visible_downloads;
    visible_downloads.ensure_capacity(AK::min(downloads.size(), max_visible_downloads_in_popover));

    for (size_t i = downloads.size(); i > 0 && visible_downloads.size() < max_visible_downloads_in_popover; --i)
        visible_downloads.append(&downloads[i - 1]);

    return visible_downloads;
}

DownloadsButtonState downloads_button_state(ReadonlySpan<FileDownloader::Download> downloads)
{
    DownloadsButtonState state;
    state.has_downloads = !downloads.is_empty();

    size_t unknown_active_download_count = 0;
    double known_downloaded_size = 0.0;
    double known_total_size = 0.0;

    for (auto const& download : downloads) {
        switch (download.status) {
        case FileDownloader::DownloadStatus::InProgress:
            ++state.active_download_count;
            if (download.total_size.has_value() && *download.total_size > 0) {
                known_downloaded_size += AK::min(download.downloaded_size, *download.total_size);
                known_total_size += *download.total_size;
            } else {
                ++unknown_active_download_count;
            }
            break;
        case FileDownloader::DownloadStatus::Paused:
            ++state.paused_download_count;
            break;
        case FileDownloader::DownloadStatus::Completed:
            ++state.completed_download_count;
            break;
        case FileDownloader::DownloadStatus::Canceled:
            break;
        case FileDownloader::DownloadStatus::Failed:
            ++state.failed_download_count;
            break;
        }
    }

    if (known_total_size > 0.0)
        state.active_download_progress = known_downloaded_size / known_total_size;

    if (state.active_download_count > 0) {
        if (state.active_download_progress.has_value() && unknown_active_download_count == 0) {
            if (state.active_download_count == 1)
                state.tooltip = MUST(String::formatted("Downloading · {}", download_percent_text(*state.active_download_progress)));
            else
                state.tooltip = MUST(String::formatted("{} downloads · {}", state.active_download_count, download_percent_text(*state.active_download_progress)));
        } else {
            state.tooltip = download_count_text(state.active_download_count, "download in progress"sv, "downloads in progress"sv);
        }
    } else if (state.failed_download_count > 0) {
        state.tooltip = download_count_text(state.failed_download_count, "download failed"sv, "downloads failed"sv);
    } else if (state.paused_download_count > 0) {
        state.tooltip = download_count_text(state.paused_download_count, "download paused"sv, "downloads paused"sv);
    } else {
        state.tooltip = "Downloads"_string;
    }

    return state;
}

}
