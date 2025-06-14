/*
 * Copyright (c) 2020, Tom Lebreux <tomlebreux@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Base64.h>
#include <string.h>

TEST_CASE(test_decode)
{
    auto decode_equal = [&](StringView input, StringView expected) {
        auto decoded = TRY_OR_FAIL(decode_base64(input));
        EXPECT_EQ(StringView { decoded }, expected);
    };

    decode_equal(""_sv, ""_sv);
    decode_equal("Zg=="_sv, "f"_sv);
    decode_equal("Zm8="_sv, "fo"_sv);
    decode_equal("Zm9v"_sv, "foo"_sv);
    decode_equal("Zm9vYg=="_sv, "foob"_sv);
    decode_equal("Zm9vYmE="_sv, "fooba"_sv);
    decode_equal("Zm9vYmFy"_sv, "foobar"_sv);
    decode_equal(" Zm9vYmFy "_sv, "foobar"_sv);
    decode_equal("  \n\r \t Zm   9v   \t YmFy \n"_sv, "foobar"_sv);

    decode_equal("aGVsbG8/d29ybGQ="_sv, "hello?world"_sv);
}

TEST_CASE(test_decode_into)
{
    ByteBuffer buffer;

    auto decode_equal = [&](StringView input, StringView expected, Optional<size_t> buffer_size = {}) {
        buffer.resize(buffer_size.value_or_lazy_evaluated([&]() {
            return AK::size_required_to_decode_base64(input);
        }));

        auto result = AK::decode_base64_into(input, buffer);
        VERIFY(!result.is_error());

        EXPECT_EQ(StringView { buffer }, expected);
    };

    decode_equal(""_sv, ""_sv);

    decode_equal("Zg=="_sv, "f"_sv);
    decode_equal("Zm8="_sv, "fo"_sv);
    decode_equal("Zm9v"_sv, "foo"_sv);
    decode_equal("Zm9vYg=="_sv, "foob"_sv);
    decode_equal("Zm9vYmE="_sv, "fooba"_sv);
    decode_equal("Zm9vYmFy"_sv, "foobar"_sv);
    decode_equal(" Zm9vYmFy "_sv, "foobar"_sv);
    decode_equal("  \n\r \t Zm   9v   \t YmFy \n"_sv, "foobar"_sv);
    decode_equal("aGVsbG8/d29ybGQ="_sv, "hello?world"_sv);

    decode_equal("Zm9vYmFy"_sv, ""_sv, 0);
    decode_equal("Zm9vYmFy"_sv, ""_sv, 1);
    decode_equal("Zm9vYmFy"_sv, ""_sv, 2);
    decode_equal("Zm9vYmFy"_sv, "foo"_sv, 3);
    decode_equal("Zm9vYmFy"_sv, "foo"_sv, 4);
    decode_equal("Zm9vYmFy"_sv, "foo"_sv, 5);
    decode_equal("Zm9vYmFy"_sv, "foobar"_sv, 6);
    decode_equal("Zm9vYmFy"_sv, "foobar"_sv, 7);
}

TEST_CASE(test_decode_invalid)
{
    EXPECT(decode_base64(("asdf\xffqwe"_sv)).is_error());
    EXPECT(decode_base64(("asdf\x80qwe"_sv)).is_error());
    EXPECT(decode_base64(("asdf:qwe"_sv)).is_error());
    EXPECT(decode_base64(("asdf=qwe"_sv)).is_error());

    EXPECT(decode_base64("aGVsbG8_d29ybGQ="_sv).is_error());
    EXPECT(decode_base64url("aGVsbG8/d29ybGQ="_sv).is_error());

    EXPECT(decode_base64("Y"_sv).is_error());
    EXPECT(decode_base64("YQ="_sv).is_error());
}

TEST_CASE(test_decode_only_padding)
{
    // Only padding is not allowed
    EXPECT(decode_base64("="_sv).is_error());
    EXPECT(decode_base64("=="_sv).is_error());
    EXPECT(decode_base64("==="_sv).is_error());
    EXPECT(decode_base64("===="_sv).is_error());

    EXPECT(decode_base64url("="_sv).is_error());
    EXPECT(decode_base64url("=="_sv).is_error());
    EXPECT(decode_base64url("==="_sv).is_error());
    EXPECT(decode_base64url("===="_sv).is_error());
}

TEST_CASE(test_encode)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64(input.bytes()));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""_sv, ""_sv);
    encode_equal("f"_sv, "Zg=="_sv);
    encode_equal("fo"_sv, "Zm8="_sv);
    encode_equal("foo"_sv, "Zm9v"_sv);
    encode_equal("foob"_sv, "Zm9vYg=="_sv);
    encode_equal("fooba"_sv, "Zm9vYmE="_sv);
    encode_equal("foobar"_sv, "Zm9vYmFy"_sv);
}

TEST_CASE(test_encode_omit_padding)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64(input.bytes(), AK::OmitPadding::Yes));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""_sv, ""_sv);
    encode_equal("f"_sv, "Zg"_sv);
    encode_equal("fo"_sv, "Zm8"_sv);
    encode_equal("foo"_sv, "Zm9v"_sv);
    encode_equal("foob"_sv, "Zm9vYg"_sv);
    encode_equal("fooba"_sv, "Zm9vYmE"_sv);
    encode_equal("foobar"_sv, "Zm9vYmFy"_sv);
}

TEST_CASE(test_urldecode)
{
    auto decode_equal = [&](StringView input, StringView expected) {
        auto decoded = TRY_OR_FAIL(decode_base64url(input));
        EXPECT_EQ(StringView { decoded }, expected);
    };

    decode_equal(""_sv, ""_sv);
    decode_equal("Zg=="_sv, "f"_sv);
    decode_equal("Zm8="_sv, "fo"_sv);
    decode_equal("Zm9v"_sv, "foo"_sv);
    decode_equal("Zm9vYg=="_sv, "foob"_sv);
    decode_equal("Zm9vYmE="_sv, "fooba"_sv);
    decode_equal("Zm9vYmFy"_sv, "foobar"_sv);
    decode_equal(" Zm9vYmFy "_sv, "foobar"_sv);
    decode_equal("  \n\r \t Zm9vYmFy \n"_sv, "foobar"_sv);

    decode_equal("TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdCwgc2VkIGRvIGVpdXNtb2QgdGVtcG9yIGluY2lkaWR1bnQgdXQgbGFib3JlIGV0IGRvbG9yZSBtYWduYSBhbGlxdWEu"_sv, "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."_sv);
    decode_equal("aGVsbG8_d29ybGQ="_sv, "hello?world"_sv);
}

TEST_CASE(test_urlencode)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64url(input.bytes()));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""_sv, ""_sv);
    encode_equal("f"_sv, "Zg=="_sv);
    encode_equal("fo"_sv, "Zm8="_sv);
    encode_equal("foo"_sv, "Zm9v"_sv);
    encode_equal("foob"_sv, "Zm9vYg=="_sv);
    encode_equal("fooba"_sv, "Zm9vYmE="_sv);
    encode_equal("foobar"_sv, "Zm9vYmFy"_sv);

    encode_equal("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."_sv, "TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdCwgc2VkIGRvIGVpdXNtb2QgdGVtcG9yIGluY2lkaWR1bnQgdXQgbGFib3JlIGV0IGRvbG9yZSBtYWduYSBhbGlxdWEu"_sv);
    encode_equal("hello?world"_sv, "aGVsbG8_d29ybGQ="_sv);

    encode_equal("hello!!world"_sv, "aGVsbG8hIXdvcmxk"_sv);
}

TEST_CASE(test_urlencode_omit_padding)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64url(input.bytes(), AK::OmitPadding::Yes));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""_sv, ""_sv);
    encode_equal("f"_sv, "Zg"_sv);
    encode_equal("fo"_sv, "Zm8"_sv);
    encode_equal("foo"_sv, "Zm9v"_sv);
    encode_equal("foob"_sv, "Zm9vYg"_sv);
    encode_equal("fooba"_sv, "Zm9vYmE"_sv);
    encode_equal("foobar"_sv, "Zm9vYmFy"_sv);

    encode_equal("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."_sv, "TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdCwgc2VkIGRvIGVpdXNtb2QgdGVtcG9yIGluY2lkaWR1bnQgdXQgbGFib3JlIGV0IGRvbG9yZSBtYWduYSBhbGlxdWEu"_sv);
    encode_equal("hello?world"_sv, "aGVsbG8_d29ybGQ"_sv);

    encode_equal("hello!!world"_sv, "aGVsbG8hIXdvcmxk"_sv);
}
