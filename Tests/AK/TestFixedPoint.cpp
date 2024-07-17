/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/FixedPoint.h>
#include <AK/NumericLimits.h>

using AK::FixedPoint;

using Type = FixedPoint<4>;

TEST_CASE(arithmetic)
{
    EXPECT_EQ(
        Type(0.5) + Type(0.5),
        Type(1));
    EXPECT_EQ(
        Type(1) + Type(0.5),
        Type(1.5));
    EXPECT_EQ(
        (float)(Type(1) + Type(0.5)),
        1.5f);
    // FIXME: Test for rounded multiply
    EXPECT_EQ(
        Type((int)1) * Type(0.5),
        Type(0.5));
    EXPECT_EQ(Type(0.125) * Type(3.75),
        Type(0.125 * 3.75));
    EXPECT_EQ(Type(0.125) * Type(-3.75),
        Type(0.125 * -3.75));

    EXPECT_EQ(
        Type((int)1) / Type(0.5),
        Type(2));
}

TEST_CASE(rounding)
{
    EXPECT_EQ(Type(0.5).rint(), Type(0));
    EXPECT_EQ(Type(0.5).floor(), Type(0));
    EXPECT_EQ(Type(0.5).ceil(), Type(1));
    EXPECT_EQ(Type(0.75).trunc(), Type(0));

    EXPECT_EQ(Type(1.5).rint(), Type(2));
    EXPECT_EQ(Type(1.5).floor(), Type(1));
    EXPECT_EQ(Type(1.5).ceil(), Type(2));
    EXPECT_EQ(Type(1.25).trunc(), Type(1));

    EXPECT_EQ(Type(-0.5).rint(), Type(0));
    EXPECT_EQ(Type(-0.5).floor(), Type(-1));
    EXPECT_EQ(Type(-0.5).ceil(), Type(0));
    EXPECT_EQ(Type(-0.75).trunc(), Type(0));

    EXPECT_EQ(Type(-1.5).rint(), Type(-2));
    EXPECT_EQ(Type(-1.5).floor(), Type(-2));
    EXPECT_EQ(Type(-1.5).ceil(), Type(-1));
    EXPECT_EQ(Type(-1.25).trunc(), Type(-1));

    EXPECT_EQ(Type(2.75).rint(), Type(3));
    EXPECT_EQ(Type(-1.25).rint(), Type(-1));

    EXPECT_EQ(Type(0.5).lrint(), 0);
    EXPECT_EQ(Type(0.5).lfloor(), 0);
    EXPECT_EQ(Type(0.5).lceil(), 1);
    EXPECT_EQ(Type(0.5).ltrunc(), 0);

    EXPECT_EQ(Type(1.5).lrint(), 2);
    EXPECT_EQ(Type(1.5).lfloor(), 1);
    EXPECT_EQ(Type(1.5).lceil(), 2);
    EXPECT_EQ(Type(1.5).ltrunc(), 1);

    EXPECT_EQ(Type(-0.5).lrint(), 0);
    EXPECT_EQ(Type(-0.5).lfloor(), -1);
    EXPECT_EQ(Type(-0.5).lceil(), 0);
    EXPECT_EQ(Type(-0.5).ltrunc(), 0);

    EXPECT_EQ(Type(-1.5).lrint(), -2);
    EXPECT_EQ(Type(-1.5).lfloor(), -2);
    EXPECT_EQ(Type(-1.5).lceil(), -1);
    EXPECT_EQ(Type(-1.5).ltrunc(), -1);

    EXPECT_EQ(Type(-1.6).rint(), -2);
    EXPECT_EQ(Type(-1.4).rint(), -1);
    EXPECT_EQ(Type(1.6).rint(), 2);
    EXPECT_EQ(Type(1.4).rint(), 1);

    // Check that sRGB TRC curve parameters match the s15fixed16 values stored in Gimp's built-in profile.
    // (This only requires that the FixedPoint<> constructor rounds before truncating to the fixed-point value,
    // as it should anyways.)
    using S15Fixed16 = FixedPoint<16, i32>;
    EXPECT_EQ(S15Fixed16(2.4).raw(), 0x26666);
    EXPECT_EQ(S15Fixed16(1 / 1.055).raw(), 0xf2a7);
    EXPECT_EQ(S15Fixed16(0.055 / 1.055).raw(), 0xd59);
    EXPECT_EQ(S15Fixed16(1 / 12.92).raw(), 0x13d0);
    EXPECT_EQ(S15Fixed16(0.04045).raw(), 0xa5b);
}

TEST_CASE(logarithm)
{
    EXPECT_EQ(Type(0).log2().raw(), NumericLimits<int>::min());
    EXPECT_EQ(Type(1).log2(), Type(0));
    EXPECT_EQ(Type(2).log2(), Type(1));
    EXPECT_EQ(Type(8).log2(), Type(3));
    EXPECT_EQ(Type(0.5).log2(), Type(-1));

    EXPECT_EQ(Type(22.627416997969520780827019587355).log2(), Type(4.4375));
    EXPECT_EQ(Type(3088).log2(), Type(11.592457037268080419637304576833));
}

TEST_CASE(comparison)
{
    EXPECT(Type(0) < 1);
    EXPECT(Type(0) <= 1);
    EXPECT(Type(0) <= 0);
    EXPECT(Type(-10) <= -10);

    EXPECT(Type(4.25) > 4);
    EXPECT(Type(4.25) >= 4);
    EXPECT(Type(4.25) <= 5);
    EXPECT(Type(4.25) < 5);
    EXPECT(Type(1.5) > 1);

    EXPECT(!(FixedPoint<4, u8>(2) > 128));
    EXPECT(!(FixedPoint<4, u8>(2) >= 128));

    EXPECT(Type(-6.25) < -6);
    EXPECT(Type(-6.25) <= -6);
    EXPECT(Type(-6.75) > -7);
    EXPECT(Type(-6.75) >= -7);

    EXPECT(Type(17) == 17);
    EXPECT(Type(-8) != -9);
}

TEST_CASE(cast)
{
    FixedPoint<16, u32> downcast_value1(FixedPoint<32, u64>(123.4567));
    EXPECT((double)downcast_value1 >= 123.4566 && (double)downcast_value1 <= 123.4568);
    static FixedPoint<32, u64> const value1(321.7654);
    downcast_value1 = value1;
    EXPECT((double)downcast_value1 >= 321.7653 && (double)downcast_value1 <= 321.7655);
    FixedPoint<6, u32> downcast_value2(FixedPoint<32, u64>(4567.123456));
    EXPECT((double)downcast_value2 >= 4567.1 && (double)downcast_value2 <= 4567.2);
    downcast_value2 = FixedPoint<32, u64>(7654.654321);
    EXPECT((double)downcast_value2 >= 7654.64 && (double)downcast_value2 <= 7654.66);

    EXPECT((double)downcast_value2 >= 7654.64 && (double)downcast_value2 <= 7654.66);
    FixedPoint<6, u32> downcast_value3(FixedPoint<32, u64>(4567.987654));
    EXPECT((double)downcast_value3 >= 4567.9 && (double)downcast_value3 <= 4567.99);
    downcast_value3 = FixedPoint<32, u64>(7654.456789);
    EXPECT((double)downcast_value3 >= 7654.45 && (double)downcast_value3 <= 7654.46);

    FixedPoint<32, u64> upcast_value1(FixedPoint<16, u32>(123.4567));
    EXPECT((double)upcast_value1 >= 123.4566 && (double)upcast_value1 <= 123.4568);
    upcast_value1 = FixedPoint<16, u32>(321.7654);
    EXPECT((double)upcast_value1 >= 321.7653 && (double)upcast_value1 <= 321.7655);
    FixedPoint<32, u64> upcast_value2(FixedPoint<6, u32>(4567.123456));
    EXPECT((double)upcast_value2 >= 4567.1 && (double)upcast_value2 <= 4567.2);
    upcast_value2 = FixedPoint<6, u32>(7654.654321);
    EXPECT((double)upcast_value2 >= 7654.64 && (double)upcast_value2 <= 7654.66);
    FixedPoint<32, u64> upcast_value3(FixedPoint<6, u32>(4567.987654));
    EXPECT((double)upcast_value3 >= 4567.9 && (double)upcast_value3 <= 4567.99);
    upcast_value3 = FixedPoint<6, u32>(7654.456789);
    EXPECT((double)upcast_value3 >= 7654.45 && (double)upcast_value3 <= 7654.46);
}

TEST_CASE(formatter)
{
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(123.456)), "123.455993"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(-123.456)), "-123.455993"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<4>(123.456)), "123.4375"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<4>(-123.456)), "-123.4375"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16> {}), "0"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(0.1)), "0.100006"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(0.02)), "0.020004"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(0.003)), "0.003005"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(0.0004)), "0.000396"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(0.0000000005)), "0"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(-0.1)), "-0.100006"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(-0.02)), "-0.020004"sv);
    EXPECT_EQ(ByteString::formatted("{}", FixedPoint<16>(-0.0000000005)), "0"sv);

    EXPECT_EQ(ByteString::formatted("{}", Type(-1)), "-1"sv);
    EXPECT_EQ(ByteString::formatted("{}", Type(-2)), "-2"sv);
    EXPECT_EQ(ByteString::formatted("{}", Type(-3)), "-3"sv);

    // exact representation
    EXPECT_EQ(ByteString::formatted("{:.30}", FixedPoint<16>(123.456)), "123.45599365234375"sv);
    EXPECT_EQ(ByteString::formatted("{:.30}", FixedPoint<16>(-0.1)), "-0.100006103515625"sv);
    EXPECT_EQ(ByteString::formatted("{:.30}", FixedPoint<16>(-0.02)), "-0.0200042724609375"sv);

    // maximum fraction per precision; 1 - 2^-precision
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<7, u64>::create_raw((1ull << 7) - 1)), "0.99218750000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<8, u64>::create_raw((1ull << 8) - 1)), "0.99609375000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<9, u64>::create_raw((1ull << 9) - 1)), "0.99804687500000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<10, u64>::create_raw((1ull << 10) - 1)), "0.99902343750000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<11, u64>::create_raw((1ull << 11) - 1)), "0.99951171875000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<12, u64>::create_raw((1ull << 12) - 1)), "0.99975585937500000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<13, u64>::create_raw((1ull << 13) - 1)), "0.99987792968750000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<14, u64>::create_raw((1ull << 14) - 1)), "0.99993896484375000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<15, u64>::create_raw((1ull << 15) - 1)), "0.99996948242187500000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<16, u64>::create_raw((1ull << 16) - 1)), "0.99998474121093750000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<17, u64>::create_raw((1ull << 17) - 1)), "0.99999237060546875000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<18, u64>::create_raw((1ull << 18) - 1)), "0.99999618530273437500"sv);
    EXPECT_EQ(ByteString::formatted("{:0.20}", AK::FixedPoint<19, u64>::create_raw((1ull << 19) - 1)), "0.99999809265136718750"sv);
    // maximum factor and precision >= 20 bits/digits will overflow u64: (5^20)*(2^20 - 1) > 2^64
    // EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<20, u64>::create_raw((1ull << 20) - 1)), "0.99999904632568359375"sv);

    // minimum fraction per precision; 2^-precision
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<7, u64>::create_raw(1)), "0.007812500000000000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<8, u64>::create_raw(1)), "0.003906250000000000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<9, u64>::create_raw(1)), "0.001953125000000000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<10, u64>::create_raw(1)), "0.000976562500000000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<11, u64>::create_raw(1)), "0.000488281250000000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<12, u64>::create_raw(1)), "0.000244140625000000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<13, u64>::create_raw(1)), "0.000122070312500000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<14, u64>::create_raw(1)), "0.000061035156250000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<15, u64>::create_raw(1)), "0.000030517578125000000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<16, u64>::create_raw(1)), "0.000015258789062500000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<17, u64>::create_raw(1)), "0.000007629394531250000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<18, u64>::create_raw(1)), "0.000003814697265625000000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<19, u64>::create_raw(1)), "0.000001907348632812500000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<20, u64>::create_raw(1)), "0.000000953674316406250000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<21, u64>::create_raw(1)), "0.000000476837158203125000000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<22, u64>::create_raw(1)), "0.000000238418579101562500000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<23, u64>::create_raw(1)), "0.000000119209289550781250000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<24, u64>::create_raw(1)), "0.000000059604644775390625000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<25, u64>::create_raw(1)), "0.000000029802322387695312500000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<26, u64>::create_raw(1)), "0.000000014901161193847656250000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<27, u64>::create_raw(1)), "0.000000007450580596923828125000"sv);
    // minimum factor and precision >= 28 bits/digits will overflow u64: (5^28)*(1) > 2^64
    // EXPECT_EQ(ByteString::formatted("{:0.30}", AK::FixedPoint<28, u64>::create_raw(1)), "0.000000003725290298461914062500"sv);

    EXPECT_EQ(ByteString::formatted("{:a}", FixedPoint<16>(42.42)), "2a.6b85"sv);
    EXPECT_EQ(ByteString::formatted("{:o}", FixedPoint<16>(42.42)), "52.327024"sv);
    EXPECT_EQ(ByteString::formatted("{:b}", FixedPoint<16>(42.42)), "101010.01101"sv);
    EXPECT_EQ(ByteString::formatted("{:0.10a}", FixedPoint<16>(69.69)), "45.b0a4000000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.10o}", FixedPoint<16>(69.69)), "105.5412200000"sv);
    EXPECT_EQ(ByteString::formatted("{:0.10b}", FixedPoint<16>(69.69)), "1000101.1011000010"sv);

    EXPECT_EQ(ByteString::formatted("{:.30o}", AK::FixedPoint<13, u64>::create_raw(1)), "0.00004"sv);
    EXPECT_EQ(ByteString::formatted("{:.30b}", AK::FixedPoint<13, u64>::create_raw(1)), "0.0000000000001"sv);
    EXPECT_EQ(ByteString::formatted("{:.30o}", AK::FixedPoint<21, u64>::create_raw(0211234567)), "21.1234567"sv);
    EXPECT_EQ(ByteString::formatted("{:.30b}", AK::FixedPoint<13, u64>::create_raw(0b110011011010110)), "11.001101101011"sv);
    EXPECT_EQ(ByteString::formatted("{:.30o}", AK::FixedPoint<11, u64>::create_raw((1ull << 11) - 1)), "0.7776"sv);
    EXPECT_EQ(ByteString::formatted("{:.30b}", AK::FixedPoint<11, u64>::create_raw((1ull << 11) - 1)), "0.11111111111"sv);

    // maximum fraction per precision rendered in hexadecimal; 1 - 2^-precision; no overflow
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<7, u64>::create_raw((1ull << 7) - 1)), "0.fe"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<8, u64>::create_raw((1ull << 8) - 1)), "0.ff"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<9, u64>::create_raw((1ull << 9) - 1)), "0.ff8"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<10, u64>::create_raw((1ull << 10) - 1)), "0.ffc"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<11, u64>::create_raw((1ull << 11) - 1)), "0.ffe"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<12, u64>::create_raw((1ull << 12) - 1)), "0.fff"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<13, u64>::create_raw((1ull << 13) - 1)), "0.fff8"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<14, u64>::create_raw((1ull << 14) - 1)), "0.fffc"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<15, u64>::create_raw((1ull << 15) - 1)), "0.fffe"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<16, u64>::create_raw((1ull << 16) - 1)), "0.ffff"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<17, u64>::create_raw((1ull << 17) - 1)), "0.ffff8"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<18, u64>::create_raw((1ull << 18) - 1)), "0.ffffc"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<19, u64>::create_raw((1ull << 19) - 1)), "0.ffffe"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<20, u64>::create_raw((1ull << 20) - 1)), "0.fffff"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<21, u64>::create_raw((1ull << 21) - 1)), "0.fffff8"sv);
    // ...skip some precisions
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<56, u64>::create_raw((1ull << 56) - 1)), "0.ffffffffffffff"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<57, u64>::create_raw((1ull << 57) - 1)), "0.ffffffffffffff8"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<58, u64>::create_raw((1ull << 58) - 1)), "0.ffffffffffffffc"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<59, u64>::create_raw((1ull << 59) - 1)), "0.ffffffffffffffe"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<60, u64>::create_raw((1ull << 60) - 1)), "0.fffffffffffffff"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<61, u64>::create_raw((1ull << 61) - 1)), "0.fffffffffffffff8"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<62, u64>::create_raw((1ull << 62) - 1)), "0.fffffffffffffffc"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<63, u64>::create_raw((1ull << 63) - 1)), "0.fffffffffffffffe"sv);

    // minimum fraction per precision rendered in hexadecimal; 2^-precision; no overflow
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<7, u64>::create_raw(1)), "0.02"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<8, u64>::create_raw(1)), "0.01"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<9, u64>::create_raw(1)), "0.008"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<10, u64>::create_raw(1)), "0.004"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<11, u64>::create_raw(1)), "0.002"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<12, u64>::create_raw(1)), "0.001"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<13, u64>::create_raw(1)), "0.0008"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<14, u64>::create_raw(1)), "0.0004"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<15, u64>::create_raw(1)), "0.0002"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<16, u64>::create_raw(1)), "0.0001"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<17, u64>::create_raw(1)), "0.00008"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<18, u64>::create_raw(1)), "0.00004"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<19, u64>::create_raw(1)), "0.00002"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<20, u64>::create_raw(1)), "0.00001"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<21, u64>::create_raw(1)), "0.000008"sv);
    // ..skip some precisions
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<56, u64>::create_raw(1)), "0.00000000000001"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<57, u64>::create_raw(1)), "0.000000000000008"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<58, u64>::create_raw(1)), "0.000000000000004"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<59, u64>::create_raw(1)), "0.000000000000002"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<60, u64>::create_raw(1)), "0.000000000000001"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<61, u64>::create_raw(1)), "0.0000000000000008"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<62, u64>::create_raw(1)), "0.0000000000000004"sv);
    EXPECT_EQ(ByteString::formatted("{:.30a}", AK::FixedPoint<63, u64>::create_raw(1)), "0.0000000000000002"sv);
}
