/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/StringUtils.h>

#include <QUrl>

TEST_CASE(qurl_conversion_preserves_percent_encoding)
{
    constexpr Array encoded_urls {
        "web+test:path%20with%20spaces"sv,
        "web+test:%E2%98%83"sv,
        "web+test:path?value=a%26b%3Dc"sv,
    };

    for (auto encoded_url : encoded_urls) {
        auto qurl = QUrl::fromEncoded(QByteArray { encoded_url.characters_without_null_termination(), static_cast<qsizetype>(encoded_url.length()) });
        EXPECT_EQ(ak_url_from_qurl(qurl).to_string(), encoded_url);
    }
}
