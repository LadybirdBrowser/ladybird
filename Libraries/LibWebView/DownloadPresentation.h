/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>
#include <LibWebView/FileDownloader.h>

namespace WebView {

struct WEBVIEW_API DownloadsButtonState {
    bool has_downloads { false };
    size_t active_download_count { 0 };
    size_t paused_download_count { 0 };
    size_t failed_download_count { 0 };
    Optional<double> active_download_progress;
    String tooltip;
};

WEBVIEW_API String download_status_text(FileDownloader::Download const&);
WEBVIEW_API String download_rate_text(u64 bytes_per_second);
WEBVIEW_API String download_time_remaining_text(AK::Duration time_remaining);
WEBVIEW_API Vector<FileDownloader::Download const*> recent_downloads_for_popover(ReadonlySpan<FileDownloader::Download>);
WEBVIEW_API DownloadsButtonState downloads_button_state(ReadonlySpan<FileDownloader::Download>);

}
