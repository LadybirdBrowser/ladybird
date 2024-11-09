/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto::ASN1 {

// ITU-T X.680, section 8, table 1
enum class Kind : u8 {
    Eol = 0x00,
    Boolean = 0x01,
    Integer = 0x02,
    BitString = 0x03,
    OctetString = 0x04,
    Null = 0x05,
    ObjectIdentifier = 0x06,
    ObjectDescriptor = 0x07,
    External = 0x08,
    Real = 0x09,
    Enumerated = 0x0A,
    EmbeddedPdv = 0x0B,
    Utf8String = 0x0C,
    RelativeOid = 0x0D,
    Time = 0x0E,
    Reserved = 0x0F,
    Sequence = 0x10,
    Set = 0x11,
    NumericString = 0x12,
    PrintableString = 0x13,
    T61String = 0x14,
    VideotexString = 0x15,
    IA5String = 0x16,
    UTCTime = 0x017,
    GeneralizedTime = 0x18,
    GraphicString = 0x19,
    VisibleString = 0x1A,
    GeneralString = 0x1B,
    UniversalString = 0x1C,
    CharacterString = 0x1D,
    BMPString = 0x1E,
    Date = 0x1F,
    TimeOfDay = 0x20,
    DateTime = 0x21,
    Duration = 0x22,
    OidIri = 0x23,
    RelativeOidIri = 0x24,
};

enum class Class : u8 {
    Universal = 0,
    Application = 0x40,
    Context = 0x80,
    Private = 0xc0,
};

enum class Type : u8 {
    Primitive = 0,
    Constructed = 0x20,
};

struct Tag {
    Kind kind;
    Class class_;
    Type type;
};

ByteString kind_name(Kind);
ByteString class_name(Class);
ByteString type_name(Type);

Optional<UnixDateTime> parse_utc_time(StringView);
Optional<UnixDateTime> parse_generalized_time(StringView);

}
