/*
 * Copyright (c) 2026, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/AuthenticationEntry.h>

namespace Web::Fetch::Infrastructure {

void set_authentication_entry(URL::URL const& url, AuthenticationEntry entry)
{
    auto key = MUST(String::formatted("{}://{}:{}", url.scheme(), url.serialized_host(), url.port_or_default()));
    s_authentication_entries.set(key, move(entry));
}

[[nodiscard]] Optional<AuthenticationEntry> get_authentication_entry(URL::URL const& url)
{
    auto const key = MUST(String::formatted("{}://{}:{}", url.scheme(), url.serialized_host(), url.port_or_default()));
    if (auto entry = s_authentication_entries.get(key); entry.has_value())
        return entry.value();
    return {};
}

}
