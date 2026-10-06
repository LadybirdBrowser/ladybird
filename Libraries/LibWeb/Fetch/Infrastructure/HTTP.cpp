/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWebCommon/Loader/LoaderConfigSnapshot.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#default-user-agent-value
ByteString default_user_agent_value(LoaderConfig const& loader_config, URL::URL const& url)
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    return loader_config.user_agent_for_url(url).to_byte_string();
}

}
