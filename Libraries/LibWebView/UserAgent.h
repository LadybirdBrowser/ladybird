/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWebView/Forward.h>

namespace WebView {

WEBVIEW_API extern OrderedHashMap<StringView, StringView> const user_agents;

WEBVIEW_API Optional<StringView> normalize_user_agent_name(StringView);

}
