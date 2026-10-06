/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/UnicodeUtils.h>
#include <LibGfx/Palette.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/SyntaxHighlighter.h>
#include <LibJS/Token.h>
#include <LibTextCodec/Decoder.h>

namespace JS {

using namespace EmbeddingABI;

static Gfx::TextAttributes style_for_token_category(Gfx::Palette const& palette, TokenCategory category)
{
    switch (category) {
    case TokenCategory::Invalid:
        return { palette.syntax_comment() };
    case TokenCategory::Number:
        return { palette.syntax_number() };
    case TokenCategory::String:
        return { palette.syntax_string() };
    case TokenCategory::Punctuation:
        return { palette.syntax_punctuation() };
    case TokenCategory::Operator:
        return { palette.syntax_operator() };
    case TokenCategory::Keyword:
        return { palette.syntax_keyword(), {}, true };
    case TokenCategory::ControlKeyword:
        return { palette.syntax_control_keyword(), {}, true };
    case TokenCategory::Identifier:
        return { palette.syntax_identifier() };
    default:
        return { palette.base_text() };
    }
}

struct RehighlightState {
    Gfx::Palette const& palette;
    Vector<Syntax::TextDocumentSpan>& spans;
    Utf16View source;
    Syntax::TextPosition position { 0, 0 };
};

static void advance_position(Syntax::TextPosition& position, Utf16View const& source, u32 start, u32 length)
{
    for (u32 i = 0; i < length; ++i) {
        if (auto code_unit = source.code_unit_at(start + i); code_unit == '\n') {
            position.set_line(position.line() + 1);
            position.set_column(0);
        } else {
            position.set_column(position.column() + 1);

            if (AK::UnicodeUtils::is_utf16_high_surrogate(code_unit)
                && i + 1 < length
                && AK::UnicodeUtils::is_utf16_low_surrogate(source.code_unit_at(start + i + 1))) {
                ++i;
            }
        }
    }
}

static void on_token(void* context, JSToken const* token)
{
    auto& state = *static_cast<RehighlightState*>(context);
    auto token_type = static_cast<TokenType>(token->token_type);
    auto category = static_cast<TokenCategory>(token->category);

    if (token->trivia_length > 0) {
        auto trivia_start = state.position;
        advance_position(state.position, state.source, token->trivia_offset, token->trivia_length);
        Syntax::TextDocumentSpan span;
        span.range.set_start(trivia_start);
        span.range.set_end({ state.position.line(), state.position.column() });
        span.attributes = style_for_token_category(state.palette, TokenCategory::Trivia);
        span.is_skippable = true;
        span.data = pack_token_data(TokenType::Trivia, TokenCategory::Trivia);
        state.spans.append(span);
    }

    auto token_start = state.position;
    if (token->length > 0) {
        advance_position(state.position, state.source, token->offset, token->length);
        Syntax::TextDocumentSpan span;
        span.range.set_start(token_start);
        span.range.set_end({ state.position.line(), state.position.column() });
        span.attributes = style_for_token_category(state.palette, category);
        span.is_skippable = false;
        span.data = pack_token_data(token_type, category);
        state.spans.append(span);
    }
}

void SyntaxHighlighter::rehighlight(Palette const& palette)
{
    auto text = m_client->get_text();
    auto decoder = TextCodec::decoder_for("UTF-8"sv);
    VERIFY(decoder.has_value());
    auto source = TextCodec::convert_input_to_utf16_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, text)
                      .release_value_but_fixme_should_propagate_errors();

    Vector<Syntax::TextDocumentSpan> spans;

    RehighlightState state {
        .palette = palette,
        .spans = spans,
        .source = source.utf16_view(),
        .position = { 0, 0 },
    };

    JSTokenSink token_sink { .context = &state, .append = on_token };
    js_compile_tokenize(utf16_view_to_abi(source.utf16_view()), &token_sink);

    m_client->do_set_spans(move(spans));
}

}
