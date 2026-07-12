/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Export.h>

namespace WebView {

WEBVIEW_API bool autocomplete_suggestions_have_same_destination(AutocompleteSuggestion const&, AutocompleteSuggestion const&);

WEBVIEW_API Vector<AutocompleteSuggestion> mux_autocomplete_suggestions(
    StringView query,
    Optional<AutocompleteSuggestion> verbatim_suggestion,
    Vector<AutocompleteSuggestion> local_suggestions,
    Vector<AutocompleteSuggestion> remote_suggestions,
    size_t limit);

}
