/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebView/Forward.h>

namespace WebView {

inline ByteString history_log_suggestions(Vector<String> const& suggestions)
{
    return ByteString::formatted("[{}]", ByteString::join(", "sv, suggestions));
}

WEBVIEW_API bool history_debug_enabled();
WEBVIEW_API ByteString history_log_entries(TraversableSessionHistory const&);
WEBVIEW_API ByteString history_log_entries(Vector<Web::HTML::SessionHistoryEntryDescriptor> const&, Optional<size_t> current_entry_index = {});
WEBVIEW_API ByteString history_log_steps(Vector<i32> const&, Optional<size_t> current_step_index = {});
WEBVIEW_API JsonObject history_json_entry(Web::HTML::SessionHistoryEntryDescriptor const&, bool current = false);
WEBVIEW_API JsonArray history_json_entries(TraversableSessionHistory const&);
WEBVIEW_API JsonArray history_json_entries(Vector<Web::HTML::SessionHistoryEntryDescriptor> const&, Optional<size_t> current_entry_index = {});
WEBVIEW_API JsonArray history_json_steps(TraversableSessionHistory const&);
WEBVIEW_API JsonArray history_json_steps(Vector<i32> const&, Optional<size_t> current_step_index = {});

}
