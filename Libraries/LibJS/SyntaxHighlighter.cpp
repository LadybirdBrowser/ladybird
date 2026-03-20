/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Utf16String.h>
#include <LibGfx/Palette.h>
#include <LibJS/SourceCode.h>
#include <LibJS/SyntaxHighlighter.h>
#include <LibJS/Token.h>

#ifdef ENABLE_RUST
#    include <LibJS/RustFFI.h>
#endif

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

bool SyntaxHighlighter::is_identifier(u64 token) const
{
    return token_type_from_packed(token) == TokenType::Identifier;
}

bool SyntaxHighlighter::is_navigatable([[maybe_unused]] u64 token) const
{
    return false;
}

struct RehighlightState {
    Gfx::Palette const& palette;
    Vector<Syntax::TextDocumentSpan>& spans;
    Vector<Syntax::TextDocumentFoldingRegion>& folding_regions;
    u16 const* source;
    Syntax::TextPosition position { 0, 0 };

    struct FoldStart {
        Syntax::TextRange range;
    };
    Vector<FoldStart> folding_region_starts;
};

static void advance_position(Syntax::TextPosition& position, u16 const* source, u32 start, u32 len)
{
    for (u32 i = 0; i < len; ++i) {
        if (source[start + i] == '\n') {
            position.set_line(position.line() + 1);
            position.set_column(0);
        } else {
            position.set_column(position.column() + 1);
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

    // Track folding regions for {} blocks
    if (token_type == TokenType::CurlyOpen) {
        state.folding_region_starts.append({ .range = { token_start, state.position } });
    } else if (token_type == TokenType::CurlyClose) {
        if (!state.folding_region_starts.is_empty()) {
            auto curly_open = state.folding_region_starts.take_last();
            Syntax::TextDocumentFoldingRegion region;
            region.range.set_start(curly_open.range.end());
            region.range.set_end(token_start);
            state.folding_regions.append(region);
        }
    }
}

void SyntaxHighlighter::rehighlight(Palette const& palette)
{
    auto text = m_client->get_text();
    auto source_utf16 = Utf16String::from_utf8(text);
    auto source_code = SourceCode::create({}, move(source_utf16));
    auto const* source_data = source_code->utf16_data();
    auto source_len = source_code->length_in_code_units();

    Vector<Syntax::TextDocumentSpan> spans;
    Vector<Syntax::TextDocumentFoldingRegion> folding_regions;

    RehighlightState state {
        .palette = palette,
        .spans = spans,
        .folding_regions = folding_regions,
        .source = source_data,
        .position = { 0, 0 },
        .folding_region_starts = {},
    };

#ifdef ENABLE_RUST
    FFI::rust_tokenize(source_data, source_len, &state,
        [](void* ctx, FFI::FFIToken const* token) { on_token(ctx, token); });
#else
    (void)source_len;
#endif

    m_client->do_set_spans(move(spans));
    m_client->do_set_folding_regions(move(folding_regions));

    m_has_brace_buddies = false;
    highlight_matching_token_pair();

    m_client->do_update();
}

Vector<Syntax::Highlighter::MatchingTokenPair> SyntaxHighlighter::matching_token_pairs_impl() const
{
    static Vector<Syntax::Highlighter::MatchingTokenPair> pairs;
    if (pairs.is_empty()) {
        pairs.append({ pack_token_data(TokenType::CurlyOpen, TokenCategory::Punctuation), pack_token_data(TokenType::CurlyClose, TokenCategory::Punctuation) });
        pairs.append({ pack_token_data(TokenType::ParenOpen, TokenCategory::Punctuation), pack_token_data(TokenType::ParenClose, TokenCategory::Punctuation) });
        pairs.append({ pack_token_data(TokenType::BracketOpen, TokenCategory::Punctuation), pack_token_data(TokenType::BracketClose, TokenCategory::Punctuation) });
    }
    return pairs;
}

bool SyntaxHighlighter::token_types_equal(u64 token1, u64 token2) const
{
    return static_cast<TokenType>(token1) == static_cast<TokenType>(token2);
}

}
