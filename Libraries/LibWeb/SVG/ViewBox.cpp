/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWeb/SVG/ViewBox.h>

namespace Web::SVG {

Optional<ViewBox> try_parse_view_box(StringView string)
{
    // FIXME: This should handle all valid viewBox values.

    GenericLexer lexer(string);

    enum State {
        MinX,
        MinY,
        Width,
        Height,
    };
    int state { State::MinX };
    ViewBox view_box;

    while (!lexer.is_eof()) {
        lexer.consume_while([](auto ch) { return is_ascii_space(ch); });
        auto token = lexer.consume_until([](auto ch) { return is_ascii_space(ch) && ch != ','; });
        auto maybe_number = token.to_number<float>();
        if (!maybe_number.has_value())
            return {};
        switch (state) {
        case State::MinX:
            view_box.min_x = maybe_number.value();
            break;
        case State::MinY:
            view_box.min_y = maybe_number.value();
            break;
        case State::Width:
            view_box.width = maybe_number.value();
            break;
        case State::Height:
            view_box.height = maybe_number.value();
            break;
        default:
            return {};
        }
        state += 1;
    }

    return view_box;
}

}
