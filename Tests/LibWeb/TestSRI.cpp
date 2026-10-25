/*
 * Copyright (c) 2026, Trail of Bits
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibWeb/SRI/SRI.h>

TEST_CASE(tab_separates_integrity_metadata)
{
    auto bytes = MUST(ByteBuffer::copy("window.H76 = true;\n"sv.bytes()));
    auto tab_metadata = Utf16String::from_utf8("\tsha256-z4UT8MNBj6eZyaDIKfDoxH50/z2Qyb4aRl9/4GpCpgA="sv);
    auto space_metadata = Utf16String::from_utf8(" sha256-z4UT8MNBj6eZyaDIKfDoxH50/z2Qyb4aRl9/4GpCpgA="sv);

    EXPECT(!MUST(Web::SRI::do_bytes_match_metadata_list(bytes, space_metadata)));
    EXPECT(!MUST(Web::SRI::do_bytes_match_metadata_list(bytes, tab_metadata)));
}
