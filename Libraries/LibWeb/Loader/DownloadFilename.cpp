/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Loader/DownloadFilename.h>

namespace Web {

ByteString sanitize_suggested_download_filename(ByteString filename)
{
    filename = LexicalPath::basename(move(filename));

    StringBuilder builder;
    for (auto byte : filename.bytes()) {
        if (byte == '\0' || byte == '/' || byte == '\\')
            builder.append('_');
        else
            builder.append(static_cast<char>(byte));
    }

    auto sanitized = builder.to_byte_string();
    if (sanitized.is_empty() || sanitized == "."sv || sanitized == ".."sv)
        return "download";
    return sanitized;
}

}
