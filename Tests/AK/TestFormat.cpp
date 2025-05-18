/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <math.h>
#include <unistd.h>

TEST_CASE(is_integral_works_properly)
{
    EXPECT(!IsIntegral<char const*>);
    EXPECT(IsIntegral<unsigned long>);
}

TEST_CASE(format_string_literals)
{
    EXPECT_EQ(ByteString::formatted("prefix-{}-suffix", "abc"), "prefix-abc-suffix");
    EXPECT_EQ(ByteString::formatted("{}{}{}", "a", "b", "c"), "abc");
}

TEST_CASE(format_integers)
{
    EXPECT_EQ(ByteString::formatted("{}", 42u), "42");
    EXPECT_EQ(ByteString::formatted("{:4}", 42u), "  42");
    EXPECT_EQ(ByteString::formatted("{:08}", 42u), "00000042");
    EXPECT_EQ(ByteString::formatted("{:7}", -17), "    -17");
    EXPECT_EQ(ByteString::formatted("{}", -17), "-17");
    EXPECT_EQ(ByteString::formatted("{:04}", 13), "0013");
    EXPECT_EQ(ByteString::formatted("{:08x}", 4096), "00001000");
    EXPECT_EQ(ByteString::formatted("{:x}", 0x1111222233334444ull), "1111222233334444");
    EXPECT_EQ(ByteString::formatted("{:4}", 12345678), "12345678");
    EXPECT_EQ(ByteString::formatted("{}", AK::NumericLimits<i64>::min()), "-9223372036854775808");
    EXPECT_EQ(ByteString::formatted("{:x}", AK::NumericLimits<i64>::min()), "-8000000000000000");
    EXPECT_EQ(ByteString::formatted("{:'}", 0), "0");
    EXPECT_EQ(ByteString::formatted("{:'}", 4096), "4,096");
    EXPECT_EQ(ByteString::formatted("{:'}", 16777216), "16,777,216");
    EXPECT_EQ(ByteString::formatted("{:'}", AK::NumericLimits<u64>::max()), "18,446,744,073,709,551,615");
    EXPECT_EQ(ByteString::formatted("{:'}", AK::NumericLimits<u64>::max()), "18,446,744,073,709,551,615");
    EXPECT_EQ(ByteString::formatted("{:'}", AK::NumericLimits<i64>::min() + 1), "-9,223,372,036,854,775,807");
    EXPECT_EQ(ByteString::formatted("{:'x}", 0), "0");
    EXPECT_EQ(ByteString::formatted("{:'x}", 16777216), "1,000,000");
    EXPECT_EQ(ByteString::formatted("{:'x}", AK::NumericLimits<u64>::max()), "f,fff,fff,fff,fff,fff");
    EXPECT_EQ(ByteString::formatted("{:'x}", AK::NumericLimits<i64>::min() + 1), "-7,fff,fff,fff,fff,fff");
    EXPECT_EQ(ByteString::formatted("{:'b}", AK::NumericLimits<u64>::max()), "1,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111,111");
}

TEST_CASE(reorder_format_arguments)
{
    EXPECT_EQ(ByteString::formatted("{1}{0}", "a", "b"), "ba");
    EXPECT_EQ(ByteString::formatted("{0}{1}", "a", "b"), "ab");
    // Compiletime check bypass: ignoring a passed argument.
    EXPECT_EQ(ByteString::formatted("{0}{0}{0}"sv, "a", "b"), "aaa");
    // Compiletime check bypass: ignoring a passed argument.
    EXPECT_EQ(ByteString::formatted("{1}{}{0}"sv, "a", "b", "c"), "baa");
}

TEST_CASE(escape_braces)
{
    EXPECT_EQ(ByteString::formatted("{{{}", "foo"), "{foo");
    EXPECT_EQ(ByteString::formatted("{}}}", "bar"), "bar}");
}

TEST_CASE(everything)
{
    EXPECT_EQ(ByteString::formatted("{{{:04}/{}/{0:8}/{1}", 42u, "foo"), "{0042/foo/      42/foo");
}

TEST_CASE(string_builder)
{
    StringBuilder builder;
    builder.appendff(" {}  ", 42);
    builder.appendff("{1}{0} ", 1, 2);

    EXPECT_EQ(builder.to_byte_string(), " 42  21 ");
}

TEST_CASE(format_without_arguments)
{
    EXPECT_EQ(ByteString::formatted("foo"), "foo");
}

TEST_CASE(format_upper_case_integer)
{
    EXPECT_EQ(ByteString::formatted("{:4X}", 0xff), "  FF");
    EXPECT_EQ(ByteString::formatted("{:#4X}", 0xff), "0XFF");

    EXPECT_EQ(ByteString::formatted("{:b}", 0xff), "11111111");
    EXPECT_EQ(ByteString::formatted("{:B}", 0xff), "11111111");
    EXPECT_EQ(ByteString::formatted("{:#b}", 0xff), "0b11111111");
}

TEST_CASE(format_aligned)
{
    EXPECT_EQ(ByteString::formatted("{:*<8}", 13), "13******");
    EXPECT_EQ(ByteString::formatted("{:*^8}", 13), "***13***");
    EXPECT_EQ(ByteString::formatted("{:*>8}", 13), "******13");
    EXPECT_EQ(ByteString::formatted("{:*>+8}", 13), "*****+13");
    EXPECT_EQ(ByteString::formatted("{:*^ 8}", 13), "** 13***");
}

TEST_CASE(format_octal)
{
    EXPECT_EQ(ByteString::formatted("{:o}", 0744), "744");
    EXPECT_EQ(ByteString::formatted("{:#o}", 0744), "0744");
    EXPECT_EQ(ByteString::formatted("{:'o}", 054321), "54,321");
    EXPECT_EQ(ByteString::formatted("{:'o}", 0567012340), "567,012,340");
}

TEST_CASE(zero_pad)
{
    EXPECT_EQ(ByteString::formatted("{: <010}", 42), "42        ");
    EXPECT_EQ(ByteString::formatted("{:010}", 42), "0000000042");
    EXPECT_EQ(ByteString::formatted("{:/^010}", 42), "////42////");
    EXPECT_EQ(ByteString::formatted("{:04x}", -32), "-0020");
    EXPECT_EQ(ByteString::formatted("{:#06x}", -64), "-0x000040");
}

TEST_CASE(replacement_field)
{
    EXPECT_EQ(ByteString::formatted("{:*>{1}}", 13, static_cast<size_t>(10)), "********13");
    EXPECT_EQ(ByteString::formatted("{:*<{1}}", 7, 4), "7***");
    // Compiletime check bypass: intentionally ignoring extra arguments
    EXPECT_EQ(ByteString::formatted("{:{2}}"sv, -5, 8, 16), "              -5");
    EXPECT_EQ(ByteString::formatted("{{{:*^{1}}}}", 1, 3), "{*1*}");
    EXPECT_EQ(ByteString::formatted("{:0{}}", 1, 3), "001");
}

TEST_CASE(replacement_field_regression)
{
    EXPECT_EQ(ByteString::formatted("{:{}}", "", static_cast<unsigned long>(6)), "      ");
}

TEST_CASE(complex_string_specifiers)
{
    EXPECT_EQ(ByteString::formatted("{:.8}", "123456789"), "12345678");
    EXPECT_EQ(ByteString::formatted("{:9}", "abcd"), "abcd     ");
    EXPECT_EQ(ByteString::formatted("{:>9}", "abcd"), "     abcd");
    EXPECT_EQ(ByteString::formatted("{:^9}", "abcd"), "  abcd   ");
    EXPECT_EQ(ByteString::formatted("{:4.6}", "a"), "a   ");
    EXPECT_EQ(ByteString::formatted("{:4.6}", "abcdef"), "abcdef");
    EXPECT_EQ(ByteString::formatted("{:4.6}", "abcdefghi"), "abcdef");
}

TEST_CASE(cast_integer_to_character)
{
    EXPECT_EQ(ByteString::formatted("{:c}", static_cast<int>('a')), "a");
    EXPECT_EQ(ByteString::formatted("{:c}", static_cast<unsigned int>('f')), "f");
}

TEST_CASE(boolean_values)
{
    EXPECT_EQ(ByteString::formatted("{}", true), "true");
    EXPECT_EQ(ByteString::formatted("{}", false), "false");
    EXPECT_EQ(ByteString::formatted("{:6}", true), "true  ");
    EXPECT_EQ(ByteString::formatted("{:>4}", false), "false");
    EXPECT_EQ(ByteString::formatted("{:d}", false), "0");
    EXPECT_EQ(ByteString::formatted("{:d}", true), "1");
    EXPECT_EQ(ByteString::formatted("{:#08x}", true), "0x00000001");
}

TEST_CASE(pointers)
{
    void* ptr = reinterpret_cast<void*>(0x4000);

    if (sizeof(void*) == 4) {
        EXPECT_EQ(ByteString::formatted("{:p}", 32), "0x00000020");
        EXPECT_EQ(ByteString::formatted("{:p}", ptr), "0x00004000");
        EXPECT_EQ(ByteString::formatted("{}", ptr), "0x00004000");
    } else if (sizeof(void*) == 8) {
        EXPECT_EQ(ByteString::formatted("{:p}", 32), "0x0000000000000020");
        EXPECT_EQ(ByteString::formatted("{:p}", ptr), "0x0000000000004000");
        EXPECT_EQ(ByteString::formatted("{}", ptr), "0x0000000000004000");
    } else {
        VERIFY_NOT_REACHED();
    }
}

// If the format implementation did absolutely nothing, all tests would pass. This
// is because when a test fails we only write "FAIL" to stdout using format.
//
// This is a bit scary, thus this test. At least this test should fail in this case.
TEST_CASE(ensure_that_format_works)
{
    if (ByteString::formatted("FAIL") != "FAIL") {
        fprintf(stderr, "FAIL\n");
        exit(1);
    }

    if (ByteString::formatted("{} FAIL {}", 1, 2) != "1 FAIL 2") {
        fprintf(stderr, "FAIL\n");
        exit(1);
    }
}

TEST_CASE(format_string_literal_as_pointer)
{
    char const* literal = "abc";
    EXPECT_EQ(ByteString::formatted("{:p}", literal), ByteString::formatted("{:p}", reinterpret_cast<FlatPtr>(literal)));
}

TEST_CASE(format_character)
{
    char a = 'a';
    EXPECT_EQ(ByteString::formatted("{}", true ? a : 'b'), "a");
}

struct A {
};
struct B {
};
template<>
struct AK::Formatter<B> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, B)
    {
        return Formatter<StringView>::format(builder, "B"sv);
    }
};

TEST_CASE(format_if_supported)
{
    EXPECT_EQ(ByteString::formatted("{}", FormatIfSupported { A {} }), "?");
    EXPECT_EQ(ByteString::formatted("{}", FormatIfSupported { B {} }), "B");
}

TEST_CASE(file_descriptor)
{
    char filename[] = "/tmp/test-file-descriptor-XXXXXX";

    int fd = mkstemp(filename);
    FILE* file = fdopen(fd, "w+");

    outln(file, "{}", "Hello, World!");
    out(file, "foo");
    outln(file, "bar");

    rewind(file);

    Array<u8, 256> buffer;
    auto const nread = fread(buffer.data(), 1, buffer.size(), file);

    EXPECT_EQ("Hello, World!\nfoobar\n"sv, StringView { buffer.span().trim(nread) });

    fclose(file);

    unlink(filename);
}

TEST_CASE(floating_point_numbers)
{
    EXPECT_EQ(ByteString::formatted("{}", 1.12), "1.12");
    EXPECT_EQ(ByteString::formatted("{}", 1.), "1");
    EXPECT_EQ(ByteString::formatted("{:.3}", 1.12), "1.12");
    EXPECT_EQ(ByteString::formatted("{:.1}", 1.12), "1.1");
    EXPECT_EQ(ByteString::formatted("{}", -1.12), "-1.12");
    EXPECT_EQ(ByteString::formatted("{:'.4}", 1234.5678), "1,234.5678");
    EXPECT_EQ(ByteString::formatted("{:'.4}", -1234.5678), "-1,234.5678");

    EXPECT_EQ(ByteString::formatted("{:.30f}", 1.0), "1.000000000000000000000000000000");
    EXPECT_EQ(ByteString::formatted("{:.30f}", 1.5), "1.500000000000000000000000000000");
    EXPECT_EQ(ByteString::formatted("{:.30f}", -2.0), "-2.000000000000000000000000000000");

    EXPECT_EQ(ByteString::formatted("{:.0f}", 1.4), "1");
    EXPECT_EQ(ByteString::formatted("{:.0f}", 1.5), "2");
    EXPECT_EQ(ByteString::formatted("{:.0f}", -1.9), "-2");

    EXPECT_EQ(ByteString::formatted("{:.1f}", 1.4), "1.4");
    EXPECT_EQ(ByteString::formatted("{:.1f}", 1.99), "2.0");
    EXPECT_EQ(ByteString::formatted("{:.1f}", 9.999), "10.0");

    EXPECT_EQ(ByteString::formatted("{}", NAN), "nan");
    EXPECT_EQ(ByteString::formatted("{}", INFINITY), "inf");
    EXPECT_EQ(ByteString::formatted("{}", -INFINITY), "-inf");

    // FIXME: There is always the question what we mean with the width field. Do we mean significant digits?
    //        Do we mean the whole width? This is what was the simplest to implement:
    EXPECT_EQ(ByteString::formatted("{:x>5.1}", 1.12), "xx1.1");
}

TEST_CASE(floating_point_default_precision)
{
#define EXPECT_FORMAT(lit, value) \
    EXPECT_EQ(ByteString::formatted("{}", lit), value##sv)

    EXPECT_FORMAT(10.3f, "10.3");
    EXPECT_FORMAT(123e4f, "1230000");
    EXPECT_FORMAT(1.23e4f, "12300");
    EXPECT_FORMAT(134232544.4365f, "134232540");
    EXPECT_FORMAT(1.43e24, "1.43e+24");
    EXPECT_FORMAT(1.43e-24, "1.43e-24");
    EXPECT_FORMAT(0.0f, "0");
    EXPECT_FORMAT(3.14159265358979, "3.14159265358979");
    EXPECT_FORMAT(1.61803399, "1.61803399");
    EXPECT_FORMAT(2.71827, "2.71827");
    EXPECT_FORMAT(42.f, "42");
    EXPECT_FORMAT(123456.78, "123456.78");
    EXPECT_FORMAT(23456.78910, "23456.7891");

#undef EXPECT_FORMAT
}

TEST_CASE(no_precision_no_trailing_number)
{
    EXPECT_EQ(ByteString::formatted("{:.0}", 0.1), "0");
}

TEST_CASE(precision_with_trailing_zeros)
{
    EXPECT_EQ(ByteString::formatted("{:0.3f}", 1.12), "1.120");
    EXPECT_EQ(ByteString::formatted("{:0.3}", 1.12), "1.12");
    EXPECT_EQ(ByteString::formatted("{:0.1}", 1.12), "1.1");
}

TEST_CASE(magnitude_less_than_zero)
{
    EXPECT_EQ(ByteString::formatted("{}", -0.654), "-0.654");
    EXPECT_EQ(ByteString::formatted("{}", 0.654), "0.654");
}

TEST_CASE(format_nullptr)
{
    EXPECT_EQ(ByteString::formatted("{}", nullptr), ByteString::formatted("{:p}", static_cast<FlatPtr>(0)));
}

struct C {
    int i;
};
template<>
struct AK::Formatter<C> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, C c)
    {
        return AK::Formatter<FormatString>::format(builder, "C(i={})"sv, c.i);
    }
};

TEST_CASE(use_format_string_formatter)
{
    EXPECT_EQ(ByteString::formatted("{:*<10}", C { 42 }), "C(i=42)***");
}

TEST_CASE(long_long_regression)
{
    EXPECT_EQ(ByteString::formatted("{}", 0x0123456789abcdefLL), "81985529216486895");

    StringBuilder builder;
    AK::FormatBuilder fmtbuilder { builder };
    MUST(fmtbuilder.put_i64(0x0123456789abcdefLL));

    EXPECT_EQ(builder.string_view(), "81985529216486895");
}

TEST_CASE(hex_dump)
{
    EXPECT_EQ(ByteString::formatted("{:hex-dump}", "0000"), "30303030");
    EXPECT_EQ(ByteString::formatted("{:>4hex-dump}", "0000"), "30303030    0000");
    EXPECT_EQ(ByteString::formatted("{:>2hex-dump}", "0000"), "3030    00\n3030    00");
    EXPECT_EQ(ByteString::formatted("{:*>4hex-dump}", "0000"), "30303030****0000");
}

TEST_CASE(span_format)
{
    {
        Vector<int> v { 1, 2, 3, 4 };
        EXPECT_EQ(ByteString::formatted("{}", v.span()), "[ 1, 2, 3, 4 ]");
        EXPECT_EQ(ByteString::formatted("{}", const_cast<AddConst<decltype(v)>&>(v).span()), "[ 1, 2, 3, 4 ]");
    }
    {
        Vector<StringView> v { "1"sv, "2"sv, "3"sv, "4"sv };
        EXPECT_EQ(ByteString::formatted("{}", v.span()), "[ 1, 2, 3, 4 ]");
        EXPECT_EQ(ByteString::formatted("{}", const_cast<AddConst<decltype(v)>&>(v).span()), "[ 1, 2, 3, 4 ]");
    }
    {
        Vector<Vector<ByteString>> v { { "1"sv, "2"sv }, { "3"sv, "4"sv } };
        EXPECT_EQ(ByteString::formatted("{}", v.span()), "[ [ 1, 2 ], [ 3, 4 ] ]");
        EXPECT_EQ(ByteString::formatted("{}", const_cast<AddConst<decltype(v)>&>(v).span()), "[ [ 1, 2 ], [ 3, 4 ] ]");
    }
}

TEST_CASE(vector_format)
{
    {
        Vector<int> v { 1, 2, 3, 4 };
        EXPECT_EQ(ByteString::formatted("{}", v), "[ 1, 2, 3, 4 ]");
    }
    {
        Vector<StringView> v { "1"sv, "2"sv, "3"sv, "4"sv };
        EXPECT_EQ(ByteString::formatted("{}", v), "[ 1, 2, 3, 4 ]");
    }
    {
        Vector<Vector<ByteString>> v { { "1"sv, "2"sv }, { "3"sv, "4"sv } };
        EXPECT_EQ(ByteString::formatted("{}", v), "[ [ 1, 2 ], [ 3, 4 ] ]");
    }
}

TEST_CASE(format_wchar)
{
    EXPECT_EQ(ByteString::formatted("{}", L'a'), "a");
    EXPECT_EQ(ByteString::formatted("{}", L'\U0001F41E'), "\xF0\x9F\x90\x9E");
    EXPECT_EQ(ByteString::formatted("{:x}", L'a'), "61");
    EXPECT_EQ(ByteString::formatted("{:x}", L'\U0001F41E'), "1f41e");
    EXPECT_EQ(ByteString::formatted("{:d}", L'a'), "97");
    EXPECT_EQ(ByteString::formatted("{:d}", L'\U0001F41E'), "128030");

    EXPECT_EQ(ByteString::formatted("{:6}", L'a'), "a     ");
    EXPECT_EQ(ByteString::formatted("{:6d}", L'a'), "    97");
    EXPECT_EQ(ByteString::formatted("{:#x}", L'\U0001F41E'), "0x1f41e");
}
