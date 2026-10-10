/*
 * Copyright (c) 2026, Trail of Bits
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibWeb/SRI/SRI.h>

TEST_CASE(uppercase_algorithm_is_normalized)
{
    auto bytes = MUST(ByteBuffer::copy("replacement response"sv.bytes()));
    auto metadata = "SHA256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="_utf16;

    auto parsed = MUST(Web::SRI::parse_metadata(metadata));
    EXPECT_EQ(parsed.size(), 1u);
    if (!parsed.is_empty())
        EXPECT_EQ(parsed.first().algorithm, "sha256"sv);
    EXPECT(!MUST(Web::SRI::do_bytes_match_metadata_list(bytes, metadata)));
}
