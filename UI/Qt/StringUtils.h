/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibURL/URL.h>

#include <QByteArray>
#include <QString>
#include <QUrl>

AK::ByteString ak_byte_string_from_qstring(QString const&);
AK::ByteString ak_byte_string_from_qbytearray(QByteArray const&);

String ak_string_from_qstring(QString const&);
QString qstring_from_ak_string(StringView);

Utf16String utf16_string_from_qstring(QString const&);
QString qstring_from_utf16_string(Utf16View const&);

QString vqformatted(StringView, AK::TypeErasedFormatParams&);

template<typename... Parameters>
QString qformatted(CheckedFormatString<Parameters...>&& format, Parameters const&... parameters)
{
    AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
    return vqformatted(format.view(), variadic_format_parameters);
}

QByteArray qbytearray_from_ak_string(StringView);

Optional<URL::URL> ak_url_from_qstring(QString const&);
URL::URL ak_url_from_qurl(QUrl const&);

namespace AK {

template<>
struct Formatter<QString> : Formatter<Utf16View> {
    ErrorOr<void> format(FormatBuilder& builder, QString const& value)
    {
        Utf16View view { reinterpret_cast<char16_t const*>(value.utf16()), static_cast<size_t>(value.size()) };
        return Formatter<Utf16View>::format(builder, view);
    }
};

}
