/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UserAgent.h"

namespace WebView {

HashMap<StringView, UserAgent> const user_agents = {
    { "Chrome Linux Desktop"sv,
        UserAgent {
            .name = "Chrome Linux Desktop"sv,
            .user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/116.0.0.0 Safari/537.36"sv,
            .sec_user_agent = "\"Not/A)Brand\";v=\"8\", \"Chromium\";v=\"116\", \"Google Chrome\";v=\"116\""sv,
            .platform = "linux"sv,
            .support_client_hints = true,
            .is_mobile = false } },
    {
        "Firefox Linux Desktop"sv,
        UserAgent {
            .name = "Firefox Linux Desktop"sv,
            .user_agent = "Mozilla/5.0 (X11; Linux x86_64; rv:109.0) Gecko/20100101 Firefox/116.0"sv,
            .sec_user_agent = ""sv,
            .platform = "linux"sv,
            .support_client_hints = false,
            .is_mobile = false },
    },
    { "Safari macOS Desktop"sv,
        UserAgent {
            .name = "Safari macOS Desktop"sv,
            .user_agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 13_5_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.5 Safari/605.1.15"sv,
            .sec_user_agent = ""sv,
            .platform = "macOS"sv,
            .support_client_hints = false,
            .is_mobile = false } },
    { "Chrome Android Mobile"sv,
        UserAgent {
            .name = "Chrome Android Mobile"sv,
            .user_agent = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/116.0.5845.114 Mobile Safari/537.36"sv,
            .sec_user_agent = "\"Not/A)Brand\";v=\"8\", \"Chromium\";v=\"116\", \"Brave\";v=\"116\""sv,
            .platform = "Android"sv,
            .support_client_hints = true,
            .is_mobile = true } },
    { "Firefox Android Mobile"sv,
        UserAgent {
            .name = "Firefox Android Mobile"sv,
            .user_agent = "Mozilla/5.0 (Android 13; Mobile; rv:109.0) Gecko/116.0 Firefox/116.0"sv,
            .sec_user_agent = ""sv,
            .platform = "Android"sv,
            .support_client_hints = false,
            .is_mobile = true } },
    { "Safari iOS Mobile"sv,
        UserAgent {
            .name = "Safari iOS Mobile"sv,
            .user_agent = "Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.0 Mobile/15E148 Safari/604.1"sv,
            .sec_user_agent = ""sv,
            .platform = "iOS"sv,
            .support_client_hints = false,
            .is_mobile = true } }
};

}
