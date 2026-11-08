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
#include <LibWebView/FileDownloader.h>

namespace Ladybird {

struct DownloadsButtonState {
    bool has_downloads { false };
    size_t active_download_count { 0 };
    size_t paused_download_count { 0 };
    size_t completed_download_count { 0 };
    size_t failed_download_count { 0 };
    Optional<double> active_download_progress;
    String tooltip;
};

String download_status_text(WebView::FileDownloader::Download const&);
String download_time_remaining_text(AK::Duration time_remaining);
Vector<WebView::FileDownloader::Download const*> recent_downloads_for_popover(ReadonlySpan<WebView::FileDownloader::Download>);
DownloadsButtonState downloads_button_state(ReadonlySpan<WebView::FileDownloader::Download>);

}
