/*
 * Copyright (c) 2022, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/StringFloatingPointConversions.h>
#include <LibTest/TestCase.h>

namespace AK {

static bool operator!=(FloatingPointExponentialForm a, FloatingPointExponentialForm b)
{
    return a.sign != b.sign || a.exponent != b.exponent || a.fraction != b.fraction;
}

}

template<>
struct AK::Formatter<AK::FloatingPointExponentialForm> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, AK::FloatingPointExponentialForm value)
    {
        return Formatter<FormatString>::format(builder, "(s={} f={} e={})"sv, value.sign, value.fraction, value.exponent);
    }
};

#define DOES_CONVERT_DOUBLE_TO(value, sign, fraction, exponent)                             \
    do {                                                                                    \
        EXPECT_EQ(                                                                          \
            convert_floating_point_to_decimal_exponential_form(static_cast<double>(value)), \
            (AK::FloatingPointExponentialForm { sign, fraction, exponent }));               \
    } while (false)

// Tests here only check basic cases. While writing, I mostly relied on the benchmarks and
// stress tests, which can be found at https://github.com/DanShaders/serenity-arithmetic-benchmark/blob/master/StringFloatingPointConversions.cpp .

TEST_CASE(double_conversion)
{
    DOES_CONVERT_DOUBLE_TO(0, 0, 0, 0);
    DOES_CONVERT_DOUBLE_TO(-0., 1, 0, 0);
    DOES_CONVERT_DOUBLE_TO(1, 0, 1, 0);
    DOES_CONVERT_DOUBLE_TO(-1, 1, 1, 0);
    DOES_CONVERT_DOUBLE_TO(.1, 0, 1, -1);
    DOES_CONVERT_DOUBLE_TO(.2, 0, 2, -1);
    DOES_CONVERT_DOUBLE_TO(.3, 0, 3, -1);
    DOES_CONVERT_DOUBLE_TO(.12345, 0, 12345, -5);
    DOES_CONVERT_DOUBLE_TO(.0012345, 0, 12345, -7);
    DOES_CONVERT_DOUBLE_TO(.1 + .2, 0, 30000000000000004, -17);
    DOES_CONVERT_DOUBLE_TO(17976931348623157e292, 0, 17976931348623157, 292);
    DOES_CONVERT_DOUBLE_TO(-17976931348623157e292, 1, 17976931348623157, 292);
    DOES_CONVERT_DOUBLE_TO(22250738585072014e-324, 0, 22250738585072014, -324);
    DOES_CONVERT_DOUBLE_TO(-22250738585072014e-324, 1, 22250738585072014, -324);
    DOES_CONVERT_DOUBLE_TO(bit_cast<double>(0xc3c04222300db8acULL), 1, 23430728857074627, 2);
}

#define DOES_CONVERT_FLOAT_TO(value, sign, fraction, exponent)                             \
    do {                                                                                   \
        EXPECT_EQ(                                                                         \
            convert_floating_point_to_decimal_exponential_form(static_cast<float>(value)), \
            (AK::FloatingPointExponentialForm { sign, fraction, exponent }));              \
    } while (false)

TEST_CASE(float_conversion)
{
    DOES_CONVERT_FLOAT_TO(0, 0, 0, 0);
    DOES_CONVERT_FLOAT_TO(-0., 1, 0, 0);
    DOES_CONVERT_FLOAT_TO(1, 0, 1, 0);
    DOES_CONVERT_FLOAT_TO(-1, 1, 1, 0);
    DOES_CONVERT_FLOAT_TO(.1, 0, 1, -1);
    DOES_CONVERT_FLOAT_TO(.2, 0, 2, -1);
    DOES_CONVERT_FLOAT_TO(.3, 0, 3, -1);
    DOES_CONVERT_FLOAT_TO(0.025, 0, 25, -3);
    DOES_CONVERT_FLOAT_TO(34028235e31, 0, 34028235, 31);
    DOES_CONVERT_FLOAT_TO(-34028235e31, 1, 34028235, 31);
    DOES_CONVERT_FLOAT_TO(11754944e-45, 0, 11754944, -45);
    DOES_CONVERT_FLOAT_TO(-11754944e-45, 1, 11754944, -45);
}
