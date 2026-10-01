/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UserAgent.h"

namespace WebView {

OrderedHashMap<StringView, StringView> const& user_agents = *new OrderedHashMap<StringView, StringView> {
    { "Chrome Linux Desktop"sv, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"sv },
    { "Chrome macOS Desktop"sv, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"sv },
    { "Firefox Linux Desktop"sv, "Mozilla/5.0 (X11; Linux x86_64; rv:156.0) Gecko/20100101 Firefox/156.0"sv },
    { "Firefox macOS Desktop"sv, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:156.0) Gecko/20100101 Firefox/156.0"sv },
    { "Safari macOS Desktop"sv, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.2 Safari/605.1.15"sv },
    { "Chrome Android Mobile"sv, "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Mobile Safari/537.36"sv },
    { "Firefox Android Mobile"sv, "Mozilla/5.0 (Android 13; Mobile; rv:156.0) Gecko/156.0 Firefox/156.0"sv },
    { "Safari iOS Mobile"sv, "Mozilla/5.0 (iPhone; CPU iPhone OS 27_2 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.2 Mobile/15E148 Safari/604.1"sv },
};

Optional<StringView> normalize_user_agent_name(StringView name)
{
    for (auto const& user_agent : user_agents) {
        if (user_agent.key.equals_ignoring_ascii_case(name))
            return user_agent.key;
    }

    return {};
}

StringView navigator_compatibility_mode_to_string(Web::NavigatorCompatibilityMode mode)
{
    switch (mode) {
    case Web::NavigatorCompatibilityMode::Chrome:
        return "chrome"sv;
    case Web::NavigatorCompatibilityMode::Gecko:
        return "gecko"sv;
    case Web::NavigatorCompatibilityMode::WebKit:
        return "webkit"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Web::NavigatorCompatibilityMode> navigator_compatibility_mode_from_string(StringView mode)
{
    if (mode == "chrome"sv)
        return Web::NavigatorCompatibilityMode::Chrome;
    if (mode == "gecko"sv)
        return Web::NavigatorCompatibilityMode::Gecko;
    if (mode == "webkit"sv)
        return Web::NavigatorCompatibilityMode::WebKit;
    return {};
}

}
