/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace AK {

template<typename T>
struct NumericLimits {
};

template<>
struct NumericLimits<bool> {
    static constexpr bool min() { return false; }
    static constexpr bool max() { return true; }
    static constexpr bool is_signed() { return false; }
    static constexpr size_t digits() { return 1; }
};

template<>
struct NumericLimits<signed char> {
    static constexpr signed char min() { return -__SCHAR_MAX__ - 1; }
    static constexpr signed char max() { return __SCHAR_MAX__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __CHAR_BIT__ - 1; }
};

template<>
struct NumericLimits<char> {
    static constexpr char min() { return -__SCHAR_MAX__ - 1; }
    static constexpr char max() { return __SCHAR_MAX__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __CHAR_BIT__ - 1; }
};

template<>
struct NumericLimits<short> {
    static constexpr short min() { return -__SHRT_MAX__ - 1; }
    static constexpr short max() { return __SHRT_MAX__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(short) - 1; }
};

template<>
struct NumericLimits<int> {
    static constexpr int min() { return -__INT_MAX__ - 1; }
    static constexpr int max() { return __INT_MAX__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(int) - 1; }
};

template<>
struct NumericLimits<long> {
    static constexpr long min() { return -__LONG_MAX__ - 1; }
    static constexpr long max() { return __LONG_MAX__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(long) - 1; }
};

template<>
struct NumericLimits<long long> {
    static constexpr long long min() { return -__LONG_LONG_MAX__ - 1; }
    static constexpr long long max() { return __LONG_LONG_MAX__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(long long) - 1; }
};

template<>
struct NumericLimits<unsigned char> {
    static constexpr unsigned char min() { return 0; }
    static constexpr unsigned char max() { return __SCHAR_MAX__ * 2u + 1; }
    static constexpr bool is_signed() { return false; }
    static constexpr size_t digits() { return __CHAR_BIT__; }
};

template<>
struct NumericLimits<unsigned short> {
    static constexpr unsigned short min() { return 0; }
    static constexpr unsigned short max() { return __SHRT_MAX__ * 2u + 1; }
    static constexpr bool is_signed() { return false; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(short); }
};

template<>
struct NumericLimits<unsigned> {
    static constexpr unsigned min() { return 0; }
    static constexpr unsigned max() { return __INT_MAX__ * 2u + 1; }
    static constexpr bool is_signed() { return false; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(int); }
};

template<>
struct NumericLimits<unsigned long> {
    static constexpr unsigned long min() { return 0; }
    static constexpr unsigned long max() { return __LONG_MAX__ * 2ul + 1; }
    static constexpr bool is_signed() { return false; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(long); }
};

template<>
struct NumericLimits<unsigned long long> {
    static constexpr unsigned long long min() { return 0; }
    static constexpr unsigned long long max() { return __LONG_LONG_MAX__ * 2ull + 1; }
    static constexpr bool is_signed() { return false; }
    static constexpr size_t digits() { return __CHAR_BIT__ * sizeof(long long); }
};

template<>
struct NumericLimits<float> {
    static constexpr float lowest() { return -__FLT_MAX__; }
    static constexpr float min_normal() { return __FLT_MIN__; }
    static constexpr float min_denormal() { return __FLT_DENORM_MIN__; }
    static constexpr float max() { return __FLT_MAX__; }
    static constexpr float epsilon() { return __FLT_EPSILON__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __FLT_MANT_DIG__; }
};

template<>
struct NumericLimits<double> {
    static constexpr double lowest() { return -__DBL_MAX__; }
    static constexpr double min_normal() { return __DBL_MIN__; }
    static constexpr double min_denormal() { return __DBL_DENORM_MIN__; }
    static constexpr double max() { return __DBL_MAX__; }
    static constexpr double epsilon() { return __DBL_EPSILON__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __DBL_MANT_DIG__; }
};

template<>
struct NumericLimits<long double> {
    static constexpr long double lowest() { return -__LDBL_MAX__; }
    static constexpr long double min_normal() { return __LDBL_MIN__; }
    static constexpr long double min_denormal() { return __LDBL_DENORM_MIN__; }
    static constexpr long double max() { return __LDBL_MAX__; }
    static constexpr long double epsilon() { return __LDBL_EPSILON__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __LDBL_MANT_DIG__; }
};

template<>
struct NumericLimits<f16> {
    static constexpr f16 lowest() { return -__FLT16_MAX__; }
    static constexpr f16 min_normal() { return __FLT16_MIN__; }
    static constexpr f16 min_denormal() { return __FLT16_DENORM_MIN__; }
    static constexpr f16 max() { return __FLT16_MAX__; }
    static constexpr f16 epsilon() { return __FLT16_EPSILON__; }
    static constexpr bool is_signed() { return true; }
    static constexpr size_t digits() { return __FLT16_MANT_DIG__; }
};

}

#if USING_AK_GLOBALLY
using AK::NumericLimits;
#endif
