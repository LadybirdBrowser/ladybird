/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/UnicodeUtils.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGfx/Palette.h>
#include <LibJS/RustFFI.h>
#include <LibJS/SourceCode.h>
#include <LibJS/SyntaxHighlighter.h>
#include <LibJS/Token.h>
#include <LibTextCodec/Decoder.h>

namespace JS {

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
    u16 const* source;
    Syntax::TextPosition position { 0, 0 };
};

static void advance_position(Syntax::TextPosition& position, u16 const* source, u32 start, u32 len)
{
    for (u32 i = 0; i < len; ++i) {
        if (auto code_unit = source[start + i]; code_unit == '\n') {
            position.set_line(position.line() + 1);
            position.set_column(0);
        } else {
            position.set_column(position.column() + 1);

            if (AK::UnicodeUtils::is_utf16_high_surrogate(code_unit)
                && i + 1 < len
                && AK::UnicodeUtils::is_utf16_low_surrogate(source[start + i + 1])) {
                ++i;
            }
        }
    }
}

static void on_token(void* ctx, FFI::FFIToken const* ffi_token)
{
    auto& state = *static_cast<RehighlightState*>(ctx);
    auto token_type = static_cast<TokenType>(ffi_token->token_type);
    auto category = static_cast<TokenCategory>(ffi_token->category);

    // Emit trivia span
    if (ffi_token->trivia_length > 0) {
        auto trivia_start = state.position;
        advance_position(state.position, state.source, ffi_token->trivia_offset, ffi_token->trivia_length);
        Syntax::TextDocumentSpan span;
        span.range.set_start(trivia_start);
        span.range.set_end({ state.position.line(), state.position.column() });
        span.attributes = style_for_token_category(state.palette, TokenCategory::Trivia);
        span.is_skippable = true;
        span.data = pack_token_data(TokenType::Trivia, TokenCategory::Trivia);
        state.spans.append(span);
    }

    // Emit token span
    auto token_start = state.position;
    if (ffi_token->length > 0) {
        advance_position(state.position, state.source, ffi_token->offset, ffi_token->length);
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
    auto source_bytes = MUST(Core::ImmutableBytes::copy(text.bytes()));
    auto decoder = TextCodec::decoder_for("UTF-8"sv);
    VERIFY(decoder.has_value());
    auto source_length = TextCodec::convert_input_to_utf16_length_using_given_decoder_unless_there_is_a_byte_order_mark(
        *decoder, StringView { source_bytes.bytes() })
                             .release_value_but_fixme_should_propagate_errors();
    auto source_code = SourceCode::create({}, source_length, "UTF-8"sv, move(source_bytes));
    auto const* source_data = source_code->utf16_data();
    auto source_len = source_code->length_in_code_units();

    Vector<Syntax::TextDocumentSpan> spans;

    RehighlightState state {
        .palette = palette,
        .spans = spans,
        .source = source_data,
        .position = { 0, 0 },
    };

    FFI::rust_tokenize(source_data, source_len, &state,
        [](void* ctx, FFI::FFIToken const* token) { on_token(ctx, token); });

    m_client->do_set_spans(move(spans));
}

}
