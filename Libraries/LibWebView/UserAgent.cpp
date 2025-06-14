/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UserAgent.h"

namespace WebView {

OrderedHashMap<StringView, StringView> const user_agents = {
    { "Chrome Linux Desktop"_sv, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36"_sv },
    { "Chrome macOS Desktop"_sv, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36"_sv },
    { "Firefox Linux Desktop"_sv, "Mozilla/5.0 (X11; Linux x86_64; rv:129.0) Gecko/20100101 Firefox/129.0"_sv },
    { "Firefox macOS Desktop"_sv, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:129.0) Gecko/20100101 Firefox/129.0"_sv },
    { "Safari macOS Desktop"_sv, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15"_sv },
    { "Chrome Android Mobile"_sv, "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/116.0.5845.114 Mobile Safari/537.36"_sv },
    { "Firefox Android Mobile"_sv, "Mozilla/5.0 (Android 13; Mobile; rv:109.0) Gecko/116.0 Firefox/116.0"_sv },
    { "Safari iOS Mobile"_sv, "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1"_sv },
};

Optional<StringView> normalize_user_agent_name(StringView name)
{
    for (auto const& user_agent : user_agents) {
        if (user_agent.key.equals_ignoring_ascii_case(name))
            return user_agent.key;
    }

    return {};
}

}
