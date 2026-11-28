/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebCommon/Loader/DownloadFilename.h>

TEST_CASE(sanitize_suggested_download_filename_replaces_invalid_utf8)
{
    EXPECT_EQ(Web::sanitize_suggested_download_filename("file-\xff\xfe.bin"sv), "file-\xef\xbf\xbd\xef\xbf\xbd.bin"sv);
}

TEST_CASE(sanitize_suggested_download_filename_keeps_valid_utf8)
{
    EXPECT_EQ(Web::sanitize_suggested_download_filename("caf\xc3\xa9.txt"sv), "caf\xc3\xa9.txt"sv);
}
