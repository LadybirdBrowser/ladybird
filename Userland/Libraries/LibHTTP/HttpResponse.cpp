/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HttpResponse.h>

namespace HTTP {

HttpResponse::HttpResponse(HttpStatus status, HeaderMap&& headers, size_t size)
    : m_status(status)
    , m_headers(move(headers))
    , m_downloaded_size(size)
{
}

}
