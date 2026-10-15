/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Impersonation.h>

namespace RequestServer {

Optional<StringView> impersonation_target_for_user_agent(StringView user_agent)
{
    // The products a User-Agent string names, from the most specific to the least: every Chromium browser's string
    // also says "Safari", and every Safari string also says "like Gecko".
    if (user_agent.contains("Firefox/"sv))
        return "firefox147"sv;
    if (user_agent.contains("Chrome/"sv) || user_agent.contains("Chromium/"sv) || user_agent.contains("CriOS/"sv))
        return user_agent.contains("Android"sv) ? "chrome131_android"sv : "chrome150"sv;
    if (user_agent.contains("Safari/"sv) && user_agent.contains("Version/"sv))
        return user_agent.contains("iPhone"sv) || user_agent.contains("iPad"sv) ? "safari260_ios"sv : "safari2601"sv;
    return {};
}

bool impersonate_user_agent([[maybe_unused]] void* curl_easy_handle, StringView user_agent)
{
    auto target = impersonation_target_for_user_agent(user_agent);
    if (!target.has_value())
        return false;

#if defined(REQUESTSERVER_CURL_IMPERSONATE)
    // The request keeps its own headers: a zero asks for the profile's handshake and HTTP/2 shape alone, not the
    // header set the profile would send on a browser's behalf.
    auto target_string = ByteString { *target };
    if (auto result = curl_easy_impersonate(static_cast<CURL*>(curl_easy_handle), target_string.characters(), 0); result != CURLE_OK) {
        dbgln("RequestServer: Failed to impersonate {}: {}", *target, curl_easy_strerror(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

}
