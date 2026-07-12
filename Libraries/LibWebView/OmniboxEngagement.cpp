/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWebView/OmniboxEngagement.h>

namespace WebView {

static String normalize_text(StringView input)
{
    auto folded = MUST(MUST(String::from_utf8(input.trim_whitespace())).to_casefold());
    StringBuilder builder;
    bool in_whitespace = false;

    for (auto code_unit : folded.bytes_as_string_view()) {
        if (is_ascii_space(code_unit)) {
            in_whitespace = true;
            continue;
        }
        if (in_whitespace && !builder.is_empty())
            builder.append(' ');
        in_whitespace = false;
        builder.append(code_unit);
    }

    return MUST(builder.to_string());
}

String normalize_omnibox_input(StringView input, OmniboxDestinationKind destination_kind)
{
    auto normalized = normalize_text(input);
    if (destination_kind == OmniboxDestinationKind::Search)
        return normalized;

    auto view = normalized.bytes_as_string_view();
    if (auto scheme_end = view.find("://"sv); scheme_end.has_value())
        view = view.substring_view(*scheme_end + 3);
    if (view.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
        view = view.substring_view(4);
    return MUST(String::from_utf8(view));
}

String normalize_omnibox_destination(StringView destination, OmniboxDestinationKind destination_kind)
{
    if (destination_kind == OmniboxDestinationKind::Search)
        return normalize_text(destination);

    auto url = URL::Parser::basic_parse(destination);
    if (!url.has_value() || !url->scheme().is_one_of("about"sv, "data"sv, "file"sv, "http"sv, "https"sv, "resource"sv))
        url = URL::Parser::basic_parse(MUST(String::formatted("https://{}", destination)));
    if (!url.has_value())
        return MUST(String::from_utf8(destination.trim_whitespace()));
    return url->serialize(URL::ExcludeFragment::Yes);
}

}
