/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <UI/Qt/DownloadPresentation.h>

TEST_CASE(time_remaining_text)
{
    auto text = [](i64 seconds) {
        return Ladybird::download_time_remaining_text(AK::Duration::from_seconds(seconds));
    };

    EXPECT_EQ(text(0), "1 second left"_string);
    EXPECT_EQ(text(45), "45 seconds left"_string);
    EXPECT_EQ(text(59), "59 seconds left"_string);

    EXPECT_EQ(text(60), "1 minute left"_string);
    EXPECT_EQ(text(61), "2 minutes left"_string);
    EXPECT_EQ(text(59 * 60), "59 minutes left"_string);

    EXPECT_EQ(text(60 * 60), "1 hour left"_string);
    EXPECT_EQ(text(61 * 60), "1 hour 1 minute left"_string);
    EXPECT_EQ(text(90 * 60), "1 hour 30 minutes left"_string);
    EXPECT_EQ(text(7199), "2 hours left"_string);
    EXPECT_EQ(text(25 * 60 * 60), "25 hours left"_string);
}

TEST_CASE(status_text)
{
    WebView::FileDownloader::Download download {
        .id = 1,
        .is_private = WebView::IsPrivate::No,
        .url = URL::about_blank(),
        .destination = LexicalPath { "/tmp/file"sv },
        .status = WebView::FileDownloader::DownloadStatus::InProgress,
        .downloaded_size = 12'400'000,
        .total_size = 19'300'000,
        .error = {},
        .can_resume = true,
        .connection_count = 1,
        .bytes_per_second = 862'500,
        .is_waiting_to_retry = false,
        .created_time = {},
    };
    EXPECT_EQ(Ladybird::download_status_text(download), "12.4 of 19.3 MB · 8 seconds left"_string);

    download.total_size = {};
    EXPECT_EQ(Ladybird::download_status_text(download), "12.4 MB downloaded"_string);

    download.status = WebView::FileDownloader::DownloadStatus::Completed;
    download.downloaded_size = 4'200'000;
    EXPECT_EQ(Ladybird::download_status_text(download), "4.2 MB"_string);
    download.downloaded_size = 512;
    EXPECT_EQ(Ladybird::download_status_text(download), "512 bytes"_string);

    download.status = WebView::FileDownloader::DownloadStatus::Failed;
    download.error = "The connection was interrupted"_string;
    EXPECT_EQ(Ladybird::download_status_text(download), "Failed · The connection was interrupted"_string);
}
