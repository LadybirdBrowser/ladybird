/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Endian.h>

static_assert(BigEndian<u32> {} == 0, "Big endian values should be default constructed in a constexpr context.");

static_assert(BigEndian<u32> { 42 } == 42, "Big endian values should be value constructed in a constexpr context.");

static_assert(LittleEndian<u32> {} == 0, "Little endian values should be default constructed in a constexpr context.");

static_assert(LittleEndian<u32> { 42 } == 42, "Little endian values should be value constructed in a constexpr context.");

enum class Enum8 : u8 { Element = 1 };
static_assert(BigEndian<Enum8> { Enum8::Element } == Enum8::Element);
static_assert(LittleEndian<Enum8> { Enum8::Element } == Enum8::Element);

enum class Enum16 : u16 { Element = 2 };
static_assert(BigEndian<Enum16> { Enum16::Element } == Enum16::Element);
static_assert(LittleEndian<Enum16> { Enum16::Element } == Enum16::Element);

enum class Enum32 : u32 { Element = 3 };
static_assert(BigEndian<Enum32> { Enum32::Element } == Enum32::Element);
static_assert(LittleEndian<Enum32> { Enum32::Element } == Enum32::Element);

enum class Enum64 : u64 { Element = 4 };
static_assert(BigEndian<Enum64> { Enum64::Element } == Enum64::Element);
static_assert(LittleEndian<Enum64> { Enum64::Element } == Enum64::Element);
