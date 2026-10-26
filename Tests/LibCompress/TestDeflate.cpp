/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCompress/Deflate.h>
#include <LibTest/TestCase.h>

TEST_CASE(invalid_canonical_code)
{
    Array<u8, 257> code;
    code.fill(0x08);
    EXPECT(Compress::CanonicalCode::from_bytes(code).is_error());
}
