/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#idl-byte
// The byte type is a signed integer type that has values in the range [−128, 127].
using Byte = i8;

// https://webidl.spec.whatwg.org/#idl-octet
// The octet type is an unsigned integer type that has values in the range [0, 255].
using Octet = u8;

// https://webidl.spec.whatwg.org/#idl-short
// The short type is a signed integer type that has values in the range [−32768, 32767].
using Short = i16;

// https://webidl.spec.whatwg.org/#idl-unsigned-short
// The unsigned short type is an unsigned integer type that has values in the range [0, 65535].
using UnsignedShort = u16;

// https://webidl.spec.whatwg.org/#idl-long
// The long type is a signed integer type that has values in the range [−2147483648, 2147483647].
using Long = i32;

// https://webidl.spec.whatwg.org/#idl-unsigned-long
// The unsigned long type is an unsigned integer type that has values in the range [0, 4294967295].
using UnsignedLong = u32;

// https://webidl.spec.whatwg.org/#idl-long-long
// The long long type is a signed integer type that has values in the range [−9223372036854775808, 9223372036854775807].
using LongLong = i64;

// https://webidl.spec.whatwg.org/#idl-unsigned-long-long
// The unsigned long long type is an unsigned integer type that has values in the range [0, 18446744073709551615].
using UnsignedLongLong = u64;

// https://webidl.spec.whatwg.org/#idl-double
// The double type is a floating point numeric type that corresponds to the set of finite
// double-precision 64-bit IEEE 754 floating point numbers. [IEEE-754]
using Double = f64;

}
