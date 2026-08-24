/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/SiteCompatibility.h>

TEST_CASE(missing_resource_directory_is_rejected)
{
    auto result = WebView::load_site_compatibility_data("file:///__ladybird_missing_site_compatibility_directory__"sv);
    EXPECT(result.is_error());
}
