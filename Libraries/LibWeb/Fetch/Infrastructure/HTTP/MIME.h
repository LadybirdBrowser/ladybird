/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

StringView legacy_extract_an_encoding(Optional<MimeSniff::MimeType> const& mime_type, StringView fallback_encoding);

}
