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

String ak_string_from_qstring(QString const& qstring)
{
    auto utf8_data = qstring.toUtf8();
    return MUST(String::from_utf8(StringView(utf8_data.data(), utf8_data.size())));
}

QString qstring_from_ak_string(StringView ak_string)
{
    return QString::fromUtf8(ak_string.characters_without_null_termination(), static_cast<qsizetype>(ak_string.length()));
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
