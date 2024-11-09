/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LoadRequest.h"
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/Page/Page.h>

namespace Web {

static int s_resource_id = 0;

LoadRequest::LoadRequest()
    : m_id(s_resource_id++)
{
}

LoadRequest LoadRequest::create_for_url_on_page(const URL::URL& url, Page* page)
{
    LoadRequest request;
    request.set_url(url);

    if (page) {
        auto cookie = page->client().page_did_request_cookie(url, Cookie::Source::Http);
        if (!cookie.is_empty())
            request.set_header("Cookie", cookie.to_byte_string());
        request.set_page(*page);
    }

    return request;
}

}
