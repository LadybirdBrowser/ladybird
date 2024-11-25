/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/UFixedBigInt.h>

namespace Web::Infra {

// https://infra.spec.whatwg.org/#booleans
// A boolean is either true or false.
using Boolean = bool;

// https://infra.spec.whatwg.org/#8-bit-unsigned-integer
// An 8-bit unsigned integer is an integer in the range 0 to 255 (0 to 2^8 − 1), inclusive.
using Unsigned8BitInteger = u8;

// https://infra.spec.whatwg.org/#16-bit-unsigned-integer
// A 16-bit unsigned integer is an integer in the range 0 to 65535 (0 to 2^16 − 1), inclusive.
using Unsigned16BitInteger = u16;

// https://infra.spec.whatwg.org/#32-bit-unsigned-integer
// A 32-bit unsigned integer is an integer in the range 0 to 4294967295 (0 to 2^32 − 1), inclusive.
using Unsigned32BitInteger = u32;

// https://infra.spec.whatwg.org/#64-bit-unsigned-integer
// A 64-bit unsigned integer is an integer in the range 0 to 18446744073709551615 (0 to 2^64 − 1), inclusive.
using Unsigned64BitInteger = u64;

// https://infra.spec.whatwg.org/#128-bit-unsigned-integer
// A 128-bit unsigned integer is an integer in the range 0 to 340282366920938463463374607431768211455 (0 to 2^128 − 1), inclusive.
using Unsigned128BitInteger = u128;

// https://infra.spec.whatwg.org/#8-bit-signed-integer
// An 8-bit signed integer is an integer in the range −128 to 127 (−2^7 to 2^7 − 1), inclusive.
using Signed8BitInteger = i8;

// https://infra.spec.whatwg.org/#16-bit-signed-integer
// A 16-bit signed integer is an integer in the range −32768 to 32767 (−2^15 to 2^15 − 1), inclusive.
using Signed16BitInteger = i16;

// https://infra.spec.whatwg.org/#32-bit-signed-integer
// A 32-bit signed integer is an integer in the range −2147483648 to 2147483647 (−2^31 to 2^31 − 1), inclusive.
using Signed32BitInteger = i32;

// https://infra.spec.whatwg.org/#64-bit-signed-integer
// A 64-bit signed integer is an integer in the range −9223372036854775808 to 9223372036854775807 (−2^63 to 2^63 − 1), inclusive.
using Signed64BitInteger = i64;

}
