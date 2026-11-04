/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Translator.h>

namespace WebView {

StringView primary_language_subtag(StringView language)
{
    if (auto separator = language.find_any_of("-_"sv); separator.has_value())
        return language.substring_view(0, *separator);
    return language;
}

bool is_same_language(StringView a, StringView b)
{
    return primary_language_subtag(a).equals_ignoring_ascii_case(primary_language_subtag(b));
}

}
