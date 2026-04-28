/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NeverDestroyed.h>
#include <AK/String.h>
#include <LibURL/URL.h>

namespace URL {

#define ENUMERATE_INTERNAL_URLS \
    __URL_ENUMERATE(bookmarks)  \
    __URL_ENUMERATE(history)    \
    __URL_ENUMERATE(newtab)     \
    __URL_ENUMERATE(processes)  \
    __URL_ENUMERATE(settings)   \
    __URL_ENUMERATE(version)

#define __URL_ENUMERATE(url)                                        \
    inline URL const& about_##url()                                 \
    {                                                               \
        static NeverDestroyed<URL> url = URL::about(#url##_string); \
        return *url;                                                \
    }
ENUMERATE_INTERNAL_URLS
#undef __URL_ENUMERATE

inline bool is_webui_url(URL const& url)
{
#define __URL_ENUMERATE(internal_url)  \
    if (url == about_##internal_url()) \
        return true;
    ENUMERATE_INTERNAL_URLS
#undef __URL_ENUMERATE

    return false;
}

}
