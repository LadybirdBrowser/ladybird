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
    TEST_TO_ASCII("www.аррӏе.com"_sv, "www.xn--80ak6aa92e.com"_sv);
    TEST_TO_ASCII("ö.com"_sv, "xn--nda.com"_sv);
    TEST_TO_ASCII("o\u0308.com"_sv, "xn--nda.com"_sv);

    // Select cases from IdnaTestV2.txt
    // FIXME: Download, parse and test all cases
    TEST_TO_ASCII("Faß.de"_sv, "xn--fa-hia.de"_sv);
    TEST_TO_ASCII_T("Faß.de"_sv, "fass.de"_sv);
    TEST_TO_ASCII("¡"_sv, "xn--7a"_sv);
    TEST_TO_ASCII("Bücher.de"_sv, "xn--bcher-kva.de");
    TEST_TO_ASCII("\u0646\u0627\u0645\u0647\u0627\u06CC"_sv, "xn--mgba3gch31f"_sv);
    TEST_TO_ASCII("A.b.c。D。"_sv, "a.b.c.d."_sv);
    TEST_TO_ASCII("βόλος"_sv, "xn--nxasmm1c"_sv);
    TEST_TO_ASCII_T("βόλος"_sv, "xn--nxasmq6b"_sv);
#undef TEST_TO_ASCII_T
#undef TEST_TO_ASCII

    EXPECT(to_ascii(Utf8View("xn--o-ccb.com"_sv)).is_error());
    EXPECT(to_ascii(Utf8View("wh--f.com"_sv)).is_error());
    EXPECT(to_ascii(Utf8View("xn--whf-cec.com"_sv)).is_error());
    EXPECT(to_ascii(Utf8View("-whf.com"_sv)).is_error());
    EXPECT(to_ascii(Utf8View("whf-.com"_sv)).is_error());
}

}
