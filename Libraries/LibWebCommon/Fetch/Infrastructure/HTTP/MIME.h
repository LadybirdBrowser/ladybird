/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibHTTP/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

WEBCOMMON_API Optional<MimeSniff::MimeType> extract_mime_type(HTTP::HeaderList const&);

}
