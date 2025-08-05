/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
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

StringView class_name(Class class_)
{
    switch (class_) {
    case Class::Application:
        return "Application"sv;
    case Class::Context:
        return "Context"sv;
    case Class::Private:
        return "Private"sv;
    case Class::Universal:
        return "Universal"sv;
    }

    return "InvalidClass"sv;
}

StringView type_name(Type type)
{
    switch (type) {
    case Type::Constructed:
        return "Constructed"sv;
    case Type::Primitive:
        return "Primitive"sv;
    }

    return "InvalidType"sv;
}

Optional<UnixDateTime> parse_utc_time(StringView time)
{
    // YYMMDDhhmm[ss]Z or YYMMDDhhmm[ss](+|-)hhmm
    GenericLexer lexer(time);
    auto year_in_century = lexer.consume(2).to_number<unsigned>();
    auto month = lexer.consume(2).to_number<unsigned>();
    auto day = lexer.consume(2).to_number<unsigned>();
    auto hour = lexer.consume(2).to_number<unsigned>();
    auto minute = lexer.consume(2).to_number<unsigned>();
    Optional<unsigned> seconds, offset_hours, offset_minutes;
    [[maybe_unused]] bool negative_offset = false;

    if (lexer.next_is(is_any_of("0123456789"sv))) {
        seconds = lexer.consume(2).to_number<unsigned>();
        if (!seconds.has_value()) {
            return {};
        }
    }

    if (lexer.next_is('Z')) {
        lexer.consume();
    } else if (lexer.next_is(is_any_of("+-"sv))) {
        negative_offset = lexer.consume() == '-';
        offset_hours = lexer.consume(2).to_number<unsigned>();
        offset_minutes = lexer.consume(2).to_number<unsigned>();
        if (!offset_hours.has_value() || !offset_minutes.has_value()) {
            return {};
        }
    } else {
        return {};
    }

    if (!year_in_century.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value()) {
        return {};
    }

    // RFC5280 section 4.1.2.5.1.
    auto full_year = year_in_century.value();
    full_year += (full_year < 50) ? 2000 : 1900;
    auto full_seconds = seconds.value_or(0);

    // FIXME: Handle offsets!
    if (offset_hours.has_value() || offset_minutes.has_value())
        dbgln("FIXME: Implement UTCTime with offset!");

    return UnixDateTime::from_unix_time_parts(full_year, month.value(), day.value(), hour.value(), minute.value(), full_seconds, 0);
}

Optional<UnixDateTime> parse_generalized_time(StringView time)
{
    // YYYYMMDDhh[mm[ss[.fff]]] or YYYYMMDDhh[mm[ss[.fff]]]Z or YYYYMMDDhh[mm[ss[.fff]]](+|-)hhmm
    GenericLexer lexer(time);
    auto year = lexer.consume(4).to_number<unsigned>();
    auto month = lexer.consume(2).to_number<unsigned>();
    auto day = lexer.consume(2).to_number<unsigned>();
    auto hour = lexer.consume(2).to_number<unsigned>();
    Optional<unsigned> minute, seconds, milliseconds, offset_hours, offset_minutes;
    [[maybe_unused]] bool negative_offset = false;

    if (!lexer.is_eof()) {
        if (lexer.consume_specific('Z'))
            goto done_parsing;

        if (!lexer.next_is(is_any_of("+-"sv))) {
            minute = lexer.consume(2).to_number<unsigned>();
            if (!minute.has_value()) {
                return {};
            }
            if (lexer.is_eof() || lexer.consume_specific('Z'))
                goto done_parsing;
        }

        if (!lexer.next_is(is_any_of("+-"sv))) {
            seconds = lexer.consume(2).to_number<unsigned>();
            if (!seconds.has_value()) {
                return {};
            }
            if (lexer.is_eof() || lexer.consume_specific('Z'))
                goto done_parsing;
        }

        if (lexer.consume_specific('.')) {
            milliseconds = lexer.consume(3).to_number<unsigned>();
            if (!milliseconds.has_value()) {
                return {};
            }
            if (lexer.is_eof() || lexer.consume_specific('Z'))
                goto done_parsing;
        }

        if (lexer.next_is(is_any_of("+-"sv))) {
            negative_offset = lexer.consume() == '-';
            offset_hours = lexer.consume(2).to_number<unsigned>();
            offset_minutes = lexer.consume(2).to_number<unsigned>();
            if (!offset_hours.has_value() || !offset_minutes.has_value()) {
                return {};
            }
        }

        // Any character would be garbage.
        if (!lexer.is_eof()) {
            return {};
        }
    }

done_parsing:;

    if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value()) {
        return {};
    }

    // FIXME: Handle offsets!
    if (offset_hours.has_value() || offset_minutes.has_value())
        dbgln("FIXME: Implement GeneralizedTime with offset!");

    return UnixDateTime::from_unix_time_parts(year.value(), month.value(), day.value(), hour.value(), minute.value_or(0), seconds.value_or(0), milliseconds.value_or(0));
}

}
