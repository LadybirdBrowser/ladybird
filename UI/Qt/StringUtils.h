/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>

#include <QString>
#include <QUrl>

AK::ByteString ak_byte_string_from_qstring(QString const&);
String ak_string_from_qstring(QString const&);
QString qstring_from_ak_string(StringView);
Optional<URL::URL> ak_url_from_qstring(QString const&);
URL::URL ak_url_from_qurl(QUrl const&);
