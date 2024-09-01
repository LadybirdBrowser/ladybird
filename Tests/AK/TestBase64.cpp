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

    decode_equal(""sv, ""sv);
    decode_equal("Zg=="sv, "f"sv);
    decode_equal("Zm8="sv, "fo"sv);
    decode_equal("Zm9v"sv, "foo"sv);
    decode_equal("Zm9vYg=="sv, "foob"sv);
    decode_equal("Zm9vYmE="sv, "fooba"sv);
    decode_equal("Zm9vYmFy"sv, "foobar"sv);
    decode_equal(" Zm9vYmFy "sv, "foobar"sv);
    decode_equal("  \n\r \t Zm   9v   \t YmFy \n"sv, "foobar"sv);

    decode_equal("aGVsbG8/d29ybGQ="sv, "hello?world"sv);
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

    decode_equal(""sv, ""sv);

    decode_equal("Zg=="sv, "f"sv);
    decode_equal("Zm8="sv, "fo"sv);
    decode_equal("Zm9v"sv, "foo"sv);
    decode_equal("Zm9vYg=="sv, "foob"sv);
    decode_equal("Zm9vYmE="sv, "fooba"sv);
    decode_equal("Zm9vYmFy"sv, "foobar"sv);
    decode_equal(" Zm9vYmFy "sv, "foobar"sv);
    decode_equal("  \n\r \t Zm   9v   \t YmFy \n"sv, "foobar"sv);
    decode_equal("aGVsbG8/d29ybGQ="sv, "hello?world"sv);

    decode_equal("Zm9vYmFy"sv, ""sv, 0);
    decode_equal("Zm9vYmFy"sv, ""sv, 1);
    decode_equal("Zm9vYmFy"sv, ""sv, 2);
    decode_equal("Zm9vYmFy"sv, "foo"sv, 3);
    decode_equal("Zm9vYmFy"sv, "foo"sv, 4);
    decode_equal("Zm9vYmFy"sv, "foo"sv, 5);
    decode_equal("Zm9vYmFy"sv, "foobar"sv, 6);
    decode_equal("Zm9vYmFy"sv, "foobar"sv, 7);
}

TEST_CASE(test_decode_invalid)
{
    EXPECT(decode_base64(("asdf\xffqwe"sv)).is_error());
    EXPECT(decode_base64(("asdf\x80qwe"sv)).is_error());
    EXPECT(decode_base64(("asdf:qwe"sv)).is_error());
    EXPECT(decode_base64(("asdf=qwe"sv)).is_error());

    EXPECT(decode_base64("aGVsbG8_d29ybGQ="sv).is_error());
    EXPECT(decode_base64url("aGVsbG8/d29ybGQ="sv).is_error());

    EXPECT(decode_base64("Y"sv).is_error());
    EXPECT(decode_base64("YQ="sv).is_error());
}

TEST_CASE(test_decode_only_padding)
{
    // Only padding is not allowed
    EXPECT(decode_base64("="sv).is_error());
    EXPECT(decode_base64("=="sv).is_error());
    EXPECT(decode_base64("==="sv).is_error());
    EXPECT(decode_base64("===="sv).is_error());

    EXPECT(decode_base64url("="sv).is_error());
    EXPECT(decode_base64url("=="sv).is_error());
    EXPECT(decode_base64url("==="sv).is_error());
    EXPECT(decode_base64url("===="sv).is_error());
}

TEST_CASE(test_encode)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64(input.bytes()));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""sv, ""sv);
    encode_equal("f"sv, "Zg=="sv);
    encode_equal("fo"sv, "Zm8="sv);
    encode_equal("foo"sv, "Zm9v"sv);
    encode_equal("foob"sv, "Zm9vYg=="sv);
    encode_equal("fooba"sv, "Zm9vYmE="sv);
    encode_equal("foobar"sv, "Zm9vYmFy"sv);
}

TEST_CASE(test_encode_omit_padding)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64(input.bytes(), AK::OmitPadding::Yes));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""sv, ""sv);
    encode_equal("f"sv, "Zg"sv);
    encode_equal("fo"sv, "Zm8"sv);
    encode_equal("foo"sv, "Zm9v"sv);
    encode_equal("foob"sv, "Zm9vYg"sv);
    encode_equal("fooba"sv, "Zm9vYmE"sv);
    encode_equal("foobar"sv, "Zm9vYmFy"sv);
}

TEST_CASE(test_urldecode)
{
    auto decode_equal = [&](StringView input, StringView expected) {
        auto decoded = TRY_OR_FAIL(decode_base64url(input));
        EXPECT_EQ(StringView { decoded }, expected);
    };

    decode_equal(""sv, ""sv);
    decode_equal("Zg=="sv, "f"sv);
    decode_equal("Zm8="sv, "fo"sv);
    decode_equal("Zm9v"sv, "foo"sv);
    decode_equal("Zm9vYg=="sv, "foob"sv);
    decode_equal("Zm9vYmE="sv, "fooba"sv);
    decode_equal("Zm9vYmFy"sv, "foobar"sv);
    decode_equal(" Zm9vYmFy "sv, "foobar"sv);
    decode_equal("  \n\r \t Zm9vYmFy \n"sv, "foobar"sv);

    decode_equal("TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdCwgc2VkIGRvIGVpdXNtb2QgdGVtcG9yIGluY2lkaWR1bnQgdXQgbGFib3JlIGV0IGRvbG9yZSBtYWduYSBhbGlxdWEu"sv, "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."sv);
    decode_equal("aGVsbG8_d29ybGQ="sv, "hello?world"sv);
}

TEST_CASE(test_urlencode)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64url(input.bytes()));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""sv, ""sv);
    encode_equal("f"sv, "Zg=="sv);
    encode_equal("fo"sv, "Zm8="sv);
    encode_equal("foo"sv, "Zm9v"sv);
    encode_equal("foob"sv, "Zm9vYg=="sv);
    encode_equal("fooba"sv, "Zm9vYmE="sv);
    encode_equal("foobar"sv, "Zm9vYmFy"sv);

    encode_equal("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."sv, "TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdCwgc2VkIGRvIGVpdXNtb2QgdGVtcG9yIGluY2lkaWR1bnQgdXQgbGFib3JlIGV0IGRvbG9yZSBtYWduYSBhbGlxdWEu"sv);
    encode_equal("hello?world"sv, "aGVsbG8_d29ybGQ="sv);

    encode_equal("hello!!world"sv, "aGVsbG8hIXdvcmxk"sv);
}

TEST_CASE(test_urlencode_omit_padding)
{
    auto encode_equal = [&](StringView input, StringView expected) {
        auto encoded = MUST(encode_base64url(input.bytes(), AK::OmitPadding::Yes));
        EXPECT_EQ(encoded, expected);
    };

    encode_equal(""sv, ""sv);
    encode_equal("f"sv, "Zg"sv);
    encode_equal("fo"sv, "Zm8"sv);
    encode_equal("foo"sv, "Zm9v"sv);
    encode_equal("foob"sv, "Zm9vYg"sv);
    encode_equal("fooba"sv, "Zm9vYmE"sv);
    encode_equal("foobar"sv, "Zm9vYmFy"sv);

    encode_equal("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."sv, "TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdCwgc2VkIGRvIGVpdXNtb2QgdGVtcG9yIGluY2lkaWR1bnQgdXQgbGFib3JlIGV0IGRvbG9yZSBtYWduYSBhbGlxdWEu"sv);
    encode_equal("hello?world"sv, "aGVsbG8_d29ybGQ"sv);

    encode_equal("hello!!world"sv, "aGVsbG8hIXdvcmxk"sv);
}
