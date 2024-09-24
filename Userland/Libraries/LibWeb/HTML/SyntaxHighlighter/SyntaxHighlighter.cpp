/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibJS/SyntaxHighlighter.h>
#include <LibJS/Token.h>
#include <LibWeb/CSS/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWeb/HTML/SyntaxHighlighter/SyntaxHighlighter.h>

namespace Web::HTML {

bool SyntaxHighlighter::is_identifier(u64 token) const
{
    if (!token)
        return false;
    return false;
}

bool SyntaxHighlighter::is_navigatable(u64) const
{
    return false;
}

void SyntaxHighlighter::rehighlight(Palette const& palette)
{
    dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(HTML::SyntaxHighlighter) starting rehighlight");
    auto text = m_client->get_text();
    clear_nested_token_pairs();

    // FIXME: Add folding regions for start and end tags.
    Vector<Syntax::TextDocumentFoldingRegion> folding_regions;
    Vector<Syntax::TextDocumentSpan> spans;
    auto highlight = [&](auto start_line, auto start_column, auto end_line, auto end_column, Gfx::TextAttributes attributes, AugmentedTokenKind kind) {
        if (start_line > end_line || (start_line == end_line && start_column >= end_column)) {
            dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(HTML::SyntaxHighlighter) discarding ({}-{}) to ({}-{}) because it has zero or negative length", start_line, start_column, end_line, end_column);
            return;
        }
        dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(HTML::SyntaxHighlighter) highlighting ({}-{}) to ({}-{}) with color {}", start_line, start_column, end_line, end_column, attributes.color);
        spans.empend(
            Syntax::TextRange {
                { start_line, start_column },
                { end_line, end_column },
            },
            move(attributes),
            static_cast<u64>(kind),
            false);
    };

    HTMLTokenizer tokenizer { text, "utf-8" };
    [[maybe_unused]] enum class State {
        HTML,
        Javascript,
        CSS,
    } state { State::HTML };
    StringBuilder substring_builder;
    Syntax::TextPosition substring_start_position;

    for (;;) {
        auto token = tokenizer.next_token();
        if (!token.has_value() || token.value().is_end_of_file())
            break;
        dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(HTML::SyntaxHighlighter) got token of type {}", token->to_string());

        if (token->is_start_tag()) {
            if (token->tag_name() == "script"sv) {
                tokenizer.switch_to(HTMLTokenizer::State::ScriptData);
                state = State::Javascript;
                substring_start_position = { token->end_position().line, token->end_position().column };
            } else if (token->tag_name() == "style"sv) {
                tokenizer.switch_to(HTMLTokenizer::State::RAWTEXT);
                state = State::CSS;
                substring_start_position = { token->end_position().line, token->end_position().column };
            }
        } else if (token->is_end_tag()) {
            if (token->tag_name().is_one_of("script"sv, "style"sv)) {
                if (state == State::Javascript) {
                    VERIFY(static_cast<u64>(AugmentedTokenKind::__Count) + first_free_token_kind_serial_value() < JS_TOKEN_START_VALUE);
                    Syntax::ProxyHighlighterClient proxy_client {
                        *m_client,
                        substring_start_position,
                        JS_TOKEN_START_VALUE,
                        substring_builder.string_view()
                    };
                    {
                        JS::SyntaxHighlighter highlighter;
                        highlighter.attach(proxy_client);
                        highlighter.rehighlight(palette);
                        highlighter.detach();
                        register_nested_token_pairs(proxy_client.corrected_token_pairs(highlighter.matching_token_pairs()));
                    }

                    spans.extend(proxy_client.corrected_spans());
                    folding_regions.extend(proxy_client.corrected_folding_regions());
                    substring_builder.clear();
                } else if (state == State::CSS) {
                    VERIFY(static_cast<u64>(AugmentedTokenKind::__Count) + first_free_token_kind_serial_value() + static_cast<u64>(JS::TokenType::_COUNT_OF_TOKENS) < CSS_TOKEN_START_VALUE);
                    Syntax::ProxyHighlighterClient proxy_client {
                        *m_client,
                        substring_start_position,
                        CSS_TOKEN_START_VALUE,
                        substring_builder.string_view()
                    };
                    {
                        CSS::SyntaxHighlighter highlighter;
                        highlighter.attach(proxy_client);
                        highlighter.rehighlight(palette);
                        highlighter.detach();
                        register_nested_token_pairs(proxy_client.corrected_token_pairs(highlighter.matching_token_pairs()));
                    }

                    spans.extend(proxy_client.corrected_spans());
                    folding_regions.extend(proxy_client.corrected_folding_regions());
                    substring_builder.clear();
                }
                state = State::HTML;
            }
        } else if (state != State::HTML) {
            VERIFY(token->is_character());
            substring_builder.append_code_point(token->code_point());
            continue;
        }

        if (token->is_comment()) {
            highlight(
                token->start_position().line,
                token->start_position().column,
                token->end_position().line,
                token->end_position().column,
                { palette.syntax_comment(), {} },
                AugmentedTokenKind::Comment);

            Syntax::TextDocumentFoldingRegion region;
            region.range.set_start({ token->start_position().line, token->start_position().column + comment_prefix()->length() });
            region.range.set_end({ token->end_position().line, token->end_position().column - comment_suffix()->length() });
            folding_regions.append(move(region));
        } else if (token->is_start_tag() || token->is_end_tag()) {
            highlight(
                token->start_position().line,
                token->start_position().column,
                token->start_position().line,
                token->start_position().column + token->tag_name().bytes().size(),
                { palette.syntax_keyword(), {}, true },
                token->is_start_tag() ? AugmentedTokenKind::OpenTag : AugmentedTokenKind::CloseTag);

            token->for_each_attribute([&](auto& attribute) {
                highlight(
                    attribute.name_start_position.line,
                    attribute.name_start_position.column,
                    attribute.name_end_position.line,
                    attribute.name_end_position.column,
                    { palette.syntax_identifier(), {} },
                    AugmentedTokenKind::AttributeName);
                highlight(
                    attribute.value_start_position.line,
                    attribute.value_start_position.column,
                    attribute.value_end_position.line,
                    attribute.value_end_position.column,
                    { palette.syntax_string(), {} },
                    AugmentedTokenKind::AttributeValue);
                return IterationDecision::Continue;
            });
        } else if (token->is_doctype()) {
            highlight(
                token->start_position().line,
                token->start_position().column,
                token->start_position().line,
                token->start_position().column,
                { palette.syntax_preprocessor_statement(), {} },
                AugmentedTokenKind::Doctype);
        }
    }

    if constexpr (SYNTAX_HIGHLIGHTING_DEBUG) {
        dbgln("(HTML::SyntaxHighlighter) list of all spans:");
        for (auto& span : spans)
            dbgln("{}, {} - {}", span.range, span.attributes.color, span.data);
        dbgln("(HTML::SyntaxHighlighter) end of list");
    }

    m_client->do_set_spans(move(spans));
    m_client->do_set_folding_regions(move(folding_regions));
    m_has_brace_buddies = false;
    highlight_matching_token_pair();
    m_client->do_update();
}

Vector<Syntax::Highlighter::MatchingTokenPair> SyntaxHighlighter::matching_token_pairs_impl() const
{
    static Vector<MatchingTokenPair> pairs;
    if (pairs.is_empty()) {
        pairs.append({ static_cast<u64>(AugmentedTokenKind::OpenTag), static_cast<u64>(AugmentedTokenKind::CloseTag) });
    }
    return pairs;
}

bool SyntaxHighlighter::token_types_equal(u64 token0, u64 token1) const
{
    return token0 == token1;
}

}
