/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <LibCore/NetworkResponse.h>
#include <LibHTTP/HeaderMap.h>
#include <LibHTTP/HttpStatus.h>

namespace HTTP {

class HttpResponse : public Core::NetworkResponse {
public:
    virtual ~HttpResponse() override = default;
    static NonnullRefPtr<HttpResponse> create(HttpStatus code, HeaderMap&& headers, size_t downloaded_size)
    {
        return adopt_ref(*new HttpResponse(code, move(headers), downloaded_size));
    }

    HttpStatus const& status() const { return m_status; }
    int code() const { return m_status.code; }
    size_t downloaded_size() const { return m_downloaded_size; }
    HeaderMap const& headers() const { return m_headers; }

private:
    HttpResponse(HttpStatus code, HeaderMap&&, size_t size);

    HttpStatus m_status;
    HeaderMap m_headers;
    size_t m_downloaded_size { 0 };
};

}
