/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/ContentBlocking.h>

namespace Web {

Optional<String> content_blocking_site(URL::URL const& url)
{
    if (!url.scheme().is_one_of("http"sv, "https"sv) || !url.host().has_value())
        return {};

    auto host = url.serialized_host();
    if (host.ends_with('.'))
        host = host.substring_view(0, host.length() - 1);
    if (host.is_empty())
        return {};

    return host.to_ascii_lowercase_string();
}

Optional<String> canonical_content_blocking_host(StringView host)
{
    if (host.is_empty())
        return {};
    if (host.contains(':') && !(host.starts_with('[') && host.ends_with(']')))
        return {};

    for (auto character : host) {
        if (is_ascii_control(character) || " /\\?#@*%"sv.contains(character))
            return {};
    }

    auto url = URL::Parser::basic_parse(MUST(String::formatted("http://{}/", host)));
    if (!url.has_value() || url->port().has_value() || !url->username().is_empty() || !url->password().is_empty())
        return {};

    return content_blocking_site(*url);
}

}
