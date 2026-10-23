/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#data-url-struct
struct DataURL {
    MimeSniff::MimeType mime_type;
    ByteBuffer body;
};

WEBCOMMON_API ErrorOr<DataURL> process_data_url(URL::URL const&);

}
