/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Infra/Types.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#idl-boolean
// The boolean type corresponds to booleans.
using Boolean = Infra::Boolean;

// https://webidl.spec.whatwg.org/#idl-byte
// The byte type corresponds to 8-bit signed integers.
using Byte = Infra::Signed8BitInteger;

// https://webidl.spec.whatwg.org/#idl-octet
// The octet type corresponds to 8-bit unsigned integers.
using Octet = Infra::Unsigned8BitInteger;

// https://webidl.spec.whatwg.org/#idl-short
// The short type corresponds to 16-bit signed integers.
using Short = Infra::Signed16BitInteger;

// https://webidl.spec.whatwg.org/#idl-unsigned-short
// The unsigned short type corresponds to 16-bit unsigned integers.
using UnsignedShort = Infra::Unsigned16BitInteger;

// https://webidl.spec.whatwg.org/#idl-long
// The long type corresponds to 32-bit signed integers.
using Long = Infra::Signed32BitInteger;

// https://webidl.spec.whatwg.org/#idl-unsigned-long
// The unsigned long type corresponds to 32-bit unsigned integers.
using UnsignedLong = Infra::Unsigned32BitInteger;

// https://webidl.spec.whatwg.org/#idl-long-long
// The long long type corresponds to 64-bit signed integers.
using LongLong = Infra::Signed64BitInteger;

// https://webidl.spec.whatwg.org/#idl-unsigned-long-long
// The unsigned long long type corresponds to 64-bit unsigned integers.
using UnsignedLongLong = Infra::Unsigned64BitInteger;

// https://webidl.spec.whatwg.org/#idl-double
// The double type is a floating point numeric type that corresponds to the set of finite
// double-precision 64-bit IEEE 754 floating point numbers. [IEEE-754]
using Double = f64;

}
