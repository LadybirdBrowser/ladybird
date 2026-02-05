/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <LibCompress/Lzw.h>

namespace {

ErrorOr<bool> test_roundtrip_string(StringView input)
{
    auto const compressed = TRY(Compress::LzwCompressor::compress_all(input.bytes(), 8));
    auto const roundtrip = TRY(Compress::LzwDecompressor<LittleEndianInputBitStream>::decompress_all(compressed, 8));
    return roundtrip == input.bytes();
}

}

TEST_CASE(roundtrip_lzw_little_endian_short)
{
    EXPECT(TRY_OR_FAIL(test_roundtrip_string("WeWellll"sv)));
}

TEST_CASE(roundtrip_lzw_little_endian_long)
{
    // LZW changes the code size after ~512 new symbols, this test case is long enough to trigger that.
    constexpr auto input = "WellWellWellWellaqwertyuiop[]sdfghjkl;'zxcvbnm,./uipnaspchu9epqrjepncdp9ruew-r8thvnufsipdonvjcx zvlrz[iu0q-348urfjsd;fjmvxc.nnnmvcxzvmc c,m;l'/,l4532[5i904tmorew;lgkrmopds['kg,l;'s,gWellWellWellWellaqwertyuiop[]sdfghjkl;'zxcvbnm,./uipnaspchu9epqrjepncdp9ruew-r8thvnufsipdonvjcx zvlrz[iu0q-348urfjsd;fjmvxc.nnnmvcxzvmc c,m;l'/,l4532[5i904tmorew;lgkrmopds['kg,l;'s,gWellWellWellWellaqwertyuiop[]sdfghjkl;'zxcvbnm,./uipnaspchu9epqrjepncdp9ruew-r8thvnufsipdonvjcx zvlrz[iu0q-348urfjsd;fjmvxc.nnnmvcxzvmc c,m;l'/,l4532[5i904tmorew;lgkrmopds['kg,l;'s,gWellWellWellWellaqwertyuiop[]sdfghjkl;'zxcvbnm,./uipnaspchu9epqrjepncdp9ruew-r8thvnufsipdonvjcx zvlrz[iu0q-348urfjsd;fjmvxc.nnnmvcxzvmc c,m;l'/,l4532[5i904tmorew;lgkrmopds['kg,l;'s,g"sv;
    EXPECT(TRY_OR_FAIL(test_roundtrip_string(input)));
}
