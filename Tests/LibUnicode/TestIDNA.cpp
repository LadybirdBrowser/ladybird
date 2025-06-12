/*
 * Copyright (c) 2023, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibUnicode/IDNA.h>

namespace Unicode::IDNA {

TEST_CASE(to_ascii)
{
#define TEST_TO_ASCII(input, expected, ...) EXPECT_EQ(TRY_OR_FAIL(to_ascii(Utf8View(input), ##__VA_ARGS__)), expected)

    ToAsciiOptions const options_with_transitional_processing {
        .transitional_processing = TransitionalProcessing::Yes
    };
#define TEST_TO_ASCII_T(input, expected) TEST_TO_ASCII(input, expected, options_with_transitional_processing)
    TEST_TO_ASCII("www.аррӏе.com"sv, "www.xn--80ak6aa92e.com"sv);
    TEST_TO_ASCII("ö.com"sv, "xn--nda.com"sv);
    TEST_TO_ASCII("o\u0308.com"sv, "xn--nda.com"sv);

    // Select cases from IdnaTestV2.txt
    // FIXME: Download, parse and test all cases
    TEST_TO_ASCII("Faß.de"sv, "xn--fa-hia.de"sv);
    TEST_TO_ASCII_T("Faß.de"sv, "fass.de"sv);
    TEST_TO_ASCII("¡"sv, "xn--7a"sv);
    TEST_TO_ASCII("Bücher.de"sv, "xn--bcher-kva.de");
    TEST_TO_ASCII("\u0646\u0627\u0645\u0647\u0627\u06CC"sv, "xn--mgba3gch31f"sv);
    TEST_TO_ASCII("A.b.c。D。"sv, "a.b.c.d."sv);
    TEST_TO_ASCII("βόλος"sv, "xn--nxasmm1c"sv);
    TEST_TO_ASCII_T("βόλος"sv, "xn--nxasmq6b"sv);
#undef TEST_TO_ASCII_T
#undef TEST_TO_ASCII

    EXPECT(to_ascii(Utf8View("xn--o-ccb.com"sv)).is_error());
    EXPECT(to_ascii(Utf8View("wh--f.com"sv)).is_error());
    EXPECT(to_ascii(Utf8View("xn--whf-cec.com"sv)).is_error());
    EXPECT(to_ascii(Utf8View("-whf.com"sv)).is_error());
    EXPECT(to_ascii(Utf8View("whf-.com"sv)).is_error());
}

}
