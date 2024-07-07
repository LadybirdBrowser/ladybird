/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/StringView.h>

namespace WebView {

struct UserAgent {
    StringView name;
    StringView user_agent;
    StringView sec_user_agent;
    StringView platform;
    bool support_client_hints;
    bool is_mobile;
};

extern HashMap<StringView, UserAgent> const user_agents;

}
