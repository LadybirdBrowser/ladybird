/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <UI/Qt/StringUtils.h>

AK::ByteString ak_byte_string_from_qstring(QString const& qstring)
{
    auto utf8_data = qstring.toUtf8();
    return AK::ByteString(utf8_data.data(), utf8_data.size());
}

AK::ByteString ak_byte_string_from_qbytearray(QByteArray const& qbytearray)
{
    return AK::ByteString(qbytearray.data(), qbytearray.size());
}

String ak_string_from_qstring(QString const& qstring)
{
    auto utf8_data = qstring.toUtf8();
    return MUST(String::from_utf8(StringView(utf8_data.data(), utf8_data.size())));
}

QString qstring_from_ak_string(StringView ak_string)
{
    return QString::fromUtf8(ak_string.characters_without_null_termination(), static_cast<qsizetype>(ak_string.length()));
}

Utf16String utf16_string_from_qstring(QString const& string)
{
    return Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(string.utf16()), static_cast<size_t>(string.size()) });
}

QString qstring_from_utf16_string(Utf16View const& string)
{
    if (string.has_ascii_storage())
        return qstring_from_ak_string(string.bytes());
    return QString::fromUtf16(string.utf16_span().data(), static_cast<qsizetype>(string.length_in_code_units()));
}

QByteArray qbytearray_from_ak_string(StringView ak_string)
{
    return { ak_string.characters_without_null_termination(), static_cast<qsizetype>(ak_string.length()) };
}

Optional<URL::URL> ak_url_from_qstring(QString const& qstring)
{
    auto utf8_data = qstring.toUtf8();
    return URL::Parser::basic_parse(StringView(utf8_data.data(), utf8_data.size()));
}

URL::URL ak_url_from_qurl(QUrl const& qurl)
{
    return ak_url_from_qstring(qurl.toString()).value();
}
