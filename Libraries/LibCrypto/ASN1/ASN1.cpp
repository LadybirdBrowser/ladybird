/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/ASN1.h>

namespace Crypto::ASN1 {

StringView kind_name(Kind kind)
{
    switch (kind) {
    case Kind::Eol:
        return "EndOfList"sv;
    case Kind::Boolean:
        return "Boolean"sv;
    case Kind::Integer:
        return "Integer"sv;
    case Kind::BitString:
        return "BitString"sv;
    case Kind::OctetString:
        return "OctetString"sv;
    case Kind::Null:
        return "Null"sv;
    case Kind::ObjectIdentifier:
        return "ObjectIdentifier"sv;
    case Kind::ObjectDescriptor:
        return "ObjectDescriptor"sv;
    case Kind::External:
        return "External"sv;
    case Kind::Real:
        return "Real"sv;
    case Kind::Enumerated:
        return "Enumerated"sv;
    case Kind::EmbeddedPdv:
        return "EmbeddedPdv"sv;
    case Kind::Utf8String:
        return "Utf8String"sv;
    case Kind::RelativeOid:
        return "RelativeOid"sv;
    case Kind::Time:
        return "Time"sv;
    case Kind::Reserved:
        return "Reserved"sv;
    case Kind::Sequence:
        return "Sequence"sv;
    case Kind::Set:
        return "Set"sv;
    case Kind::NumericString:
        return "NumericString"sv;
    case Kind::PrintableString:
        return "PrintableString"sv;
    case Kind::T61String:
        return "T61String"sv;
    case Kind::VideotexString:
        return "VideotexString"sv;
    case Kind::IA5String:
        return "IA5String"sv;
    case Kind::UTCTime:
        return "UTCTime"sv;
    case Kind::GeneralizedTime:
        return "GeneralizedTime"sv;
    case Kind::GraphicString:
        return "GraphicString"sv;
    case Kind::VisibleString:
        return "VisibleString"sv;
    case Kind::GeneralString:
        return "GeneralString"sv;
    case Kind::UniversalString:
        return "UniversalString"sv;
    case Kind::CharacterString:
        return "CharacterString"sv;
    case Kind::BMPString:
        return "BMPString"sv;
    case Kind::Date:
        return "Date"sv;
    case Kind::TimeOfDay:
        return "TimeOfDay"sv;
    case Kind::DateTime:
        return "DateTime"sv;
    case Kind::Duration:
        return "Duration"sv;
    case Kind::OidIri:
        return "OidIri"sv;
    case Kind::RelativeOidIri:
        return "RelativeOidIri"sv;
    }

    return "InvalidKind"sv;
}

}
