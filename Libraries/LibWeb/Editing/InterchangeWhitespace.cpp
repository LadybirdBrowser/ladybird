/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/InterchangeWhitespace.h>

namespace Web::Editing {

bool is_ascii_whitespace_text(DOM::Node const& node)
{
    auto const* text = as_if<DOM::Text>(node);
    if (!text)
        return false;
    return all_of(text->data().utf16_view(), is_ascii_space);
}

Vector<InterchangeWhitespaceToken> rebalance_whitespace_for_interchange(Utf16View input)
{
    Vector<InterchangeWhitespaceToken> tokens;
    Utf16StringBuilder pending_text;
    auto flush_text = [&] {
        if (pending_text.is_empty())
            return;
        tokens.append({ InterchangeWhitespaceTokenType::Text, pending_text.to_string() });
        pending_text.clear();
    };
    auto append_protected_space = [&] {
        flush_text();
        tokens.append({ InterchangeWhitespaceTokenType::ProtectedSpace, {} });
    };
    auto is_collapsible_whitespace = [](u16 code_unit) {
        return code_unit == ' ' || code_unit == '\n';
    };

    size_t index = 0;
    while (index < input.length_in_code_units()) {
        if (!is_collapsible_whitespace(input.code_unit_at(index))) {
            pending_text.append_code_point(input.code_unit_at(index++));
            continue;
        }

        auto start = index;
        while (index < input.length_in_code_units() && is_collapsible_whitespace(input.code_unit_at(index)))
            ++index;
        auto count = index - start;
        while (count > 0) {
            auto add = count % 3;
            if (add == 0) {
                append_protected_space();
                pending_text.append_ascii(' ');
                append_protected_space();
                add = 3;
            } else if (add == 1) {
                if (start == 0 || start + 1 == input.length_in_code_units())
                    append_protected_space();
                else
                    pending_text.append_ascii(' ');
            } else {
                if (start == 0) {
                    append_protected_space();
                    pending_text.append_ascii(' ');
                } else if (start + 2 == input.length_in_code_units()) {
                    append_protected_space();
                    append_protected_space();
                } else {
                    append_protected_space();
                    pending_text.append_ascii(' ');
                }
            }
            count -= add;
        }
    }
    flush_text();
    return tokens;
}

}
