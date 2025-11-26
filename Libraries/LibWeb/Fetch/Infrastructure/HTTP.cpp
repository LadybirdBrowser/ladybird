/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#default-user-agent-value
ByteString const& default_user_agent_value()
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    static auto user_agent = ResourceLoader::the().user_agent().to_byte_string();
    return user_agent;
}

}
